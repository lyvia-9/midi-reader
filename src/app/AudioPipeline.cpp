#include "app/AudioPipeline.h"

namespace reader {

static juce::String U(const char* s) { return juce::String::fromUTF8(s); }

static const char* const audioExtensions[] = {"wav", "mp3", "flac", "ogg", "m4a", "aif", "aiff", "opus"};

bool isAudioFile(const juce::File& f) {
    const auto ext = f.getFileExtension().trimCharactersAtStart(".").toLowerCase();
    for (auto* e : audioExtensions)
        if (ext == e) return true;
    return false;
}

juce::String audioFileWildcard() {
    juce::StringArray parts;
    for (auto* e : audioExtensions) parts.add(juce::String("*.") + e);
    return parts.joinIntoString(";");
}

// ---------------------------------------------------------------------------

PipelineLocation PipelineLocation::at(const juce::File& dir) {
    PipelineLocation l;
    if (dir.getChildFile("pipeline_worker.py").existsAsFile()) l.protoDir = dir;
    else if (dir.getChildFile("proto/pipeline_worker.py").existsAsFile()) l.protoDir = dir.getChildFile("proto");
    else {
        l.problem = U("这个文件夹里没有音频管线（找不到 proto/pipeline_worker.py）：") + dir.getFullPathName();
        return l;
    }
#if JUCE_WINDOWS
    l.python = l.protoDir.getChildFile(".venv/Scripts/python.exe");
    const auto setup = U("proto\\setup.bat");
#else
    l.python = l.protoDir.getChildFile(".venv/bin/python");
    const auto setup = U("proto/README.md 里的安装步骤");
#endif
    if (!l.python.existsAsFile())
        l.problem = U("proto 里还没有 Python 环境（") + l.python.getFullPathName() + U("）。先运行一次 ") + setup + U("。");
    return l;
}

PipelineLocation PipelineLocation::find(juce::PropertySet* settings) {
    if (settings != nullptr) {
        const auto chosen = settings->getValue("pipelineDir");
        if (chosen.isNotEmpty()) return at(juce::File(chosen));
    }
    const auto env = juce::SystemStats::getEnvironmentVariable("MIDI_READER_PROTO", {});
    if (env.isNotEmpty()) return at(juce::File(env));
    // 从仓库里编出来的 exe 在 build/MidiReader_artefacts/Release/ 下，往上找仓库根
    auto d = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 8 && d.exists(); ++i, d = d.getParentDirectory()) {
        if (d.getChildFile("proto/pipeline_worker.py").existsAsFile() || d.getChildFile("pipeline_worker.py").existsAsFile())
            return at(d);
        if (d.isRoot()) break;
    }
    PipelineLocation l;
    l.problem = U("找不到音频管线。扒谱要用仓库里的 proto 文件夹（模型在 Python 那边跑），请选一下仓库或 proto 文件夹。");
    return l;
}

// ---------------------------------------------------------------------------

PipelineJob::PipelineJob(PipelineLocation l, juce::String m, juce::var p, Callbacks c)
    : juce::Thread("pipeline job"), loc(std::move(l)), meth(std::move(m)), params(std::move(p)), cb(std::move(c)) {
    auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("midi-reader-jobs");
    // 顺手清掉一周前的任务目录（日志留一阵，出错时还能看）
    for (auto& old : root.findChildFiles(juce::File::findDirectories, false))
        if (old.getLastModificationTime() < juce::Time::getCurrentTime() - juce::RelativeTime::days(7))
            old.deleteRecursively();
    dir = root.getChildFile(juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S-") + juce::Uuid().toString().substring(0, 8));
}

PipelineJob::~PipelineJob() {
    cancelPendingUpdate();
    if (isThreadRunning()) {
        cancel();
        // 管线收到取消会先停掉它起的模型进程；等不到就只能强杀 Python 了
        if (!waitForThreadToExit(4000)) {
            process.kill();
            stopThread(2000);
        }
    }
}

bool PipelineJob::start() {
    if (!loc.ok()) { startErr = loc.problem; return false; }
    if (!dir.createDirectory()) { startErr = U("建不了临时目录：") + dir.getFullPathName(); return false; }
    auto* req = new juce::DynamicObject();
    req->setProperty("method", meth);
    req->setProperty("params", params);
    req->setProperty("cancelFile", dir.getChildFile("cancel").getFullPathName());
    req->setProperty("logFile", logFile().getFullPathName());
    const auto requestFile = dir.getChildFile("request.json");
    if (!requestFile.replaceWithText(juce::JSON::toString(juce::var(req)), false, false, "\n")) {
        startErr = U("写不了请求文件：") + requestFile.getFullPathName();
        return false;
    }
    juce::StringArray args{loc.python.getFullPathName(), "-X", "utf8",
                           loc.protoDir.getChildFile("pipeline_worker.py").getFullPathName(),
                           "job", requestFile.getFullPathName()};
    if (!process.start(args, juce::ChildProcess::wantStdOut)) {
        startErr = U("起不来 Python：") + loc.python.getFullPathName();
        return false;
    }
    startThread();
    return true;
}

void PipelineJob::cancel() {
    dir.getChildFile("cancel").create();
}

juce::String PipelineJob::logTail(int lines) const {
    juce::StringArray all;
    all.addLines(logFile().loadFileAsString());
    all.removeEmptyStrings();
    if (all.size() > lines) all.removeRange(0, all.size() - lines);
    return all.joinIntoString("\n");
}

void PipelineJob::run() {
    // ChildProcess::readProcessOutput 要读满才返回，一次只读一个字节，事件才能马上到
    std::string line;
    char c = 0;
    for (;;) {
        if (process.readProcessOutput(&c, 1) != 1) {
            if (!process.isRunning()) break;
            juce::Thread::sleep(20);
            continue;
        }
        if (c != '\n') { line.push_back(c); continue; }
        const juce::ScopedLock sl(lock);
        pending.add(juce::String::fromUTF8(line.data(), (int)line.size()).trimEnd());
        line.clear();
        triggerAsyncUpdate();
    }
    const juce::ScopedLock sl(lock);
    if (!line.empty()) pending.add(juce::String::fromUTF8(line.data(), (int)line.size()));
    exited = true;
    triggerAsyncUpdate();
}

void PipelineJob::handleAsyncUpdate() {
    juce::StringArray lines;
    bool done;
    {
        const juce::ScopedLock sl(lock);
        lines.swapWith(pending);
        done = exited;
    }
    for (auto& l : lines) {
        if (!l.startsWithChar('{')) continue;
        const auto msg = juce::JSON::parse(l);
        if (!msg.isObject()) continue;
        if (msg.hasProperty("result") || msg.hasProperty("error")) gotReply = true;
        if (cb.message) cb.message(msg);
    }
    if (done && !finishedSent) {
        finishedSent = true;
        if (!gotReply && cb.message) {
            auto* err = new juce::DynamicObject();
            err->setProperty("type", "internal");
            auto text = U("管线进程退出了，但没有给出结果（退出码 ") + juce::String((int)process.getExitCode()) + U("）。");
            const auto tail = logTail();
            if (tail.isNotEmpty()) text << U("\n\n日志最后几行：\n") << tail;
            err->setProperty("message", text);
            auto* msg = new juce::DynamicObject();
            msg->setProperty("error", juce::var(err));
            cb.message(juce::var(msg));
        }
        if (cb.finished) cb.finished();   // 可能在这里把自己删掉，之后不能再碰成员
    }
}

} // namespace reader
