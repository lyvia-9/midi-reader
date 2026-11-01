// 音频管线（proto/reader_proto/pipeline）的进程外调用。模型都在 Python 那边跑，这里只起进程、读事件。
//
// JUCE 的 ChildProcess 只能读子进程的 stdout，不能写 stdin、不能设工作目录和环境变量，所以用管线的
// 单次模式：`python -X utf8 proto/pipeline_worker.py job <request.json>`。请求、取消标记、日志都是
// 一个临时任务目录里的文件；stdout 一行一个 JSON（协议见 docs/audio-pipeline.md 第 11 节）。
#pragma once
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

namespace reader {

bool isAudioFile(const juce::File&);
juce::String audioFileWildcard();   // 给 FileChooser

// 管线在哪：proto 目录和它的虚拟环境里的 Python。
struct PipelineLocation {
    juce::File protoDir;   // 含 pipeline_worker.py
    juce::File python;
    juce::String problem;  // 空 = 能用

    bool ok() const { return problem.isEmpty(); }
    // 依次看：用户选过的目录（settings 里的 pipelineDir）、环境变量 MIDI_READER_PROTO、exe 往上几层里的 proto/
    static PipelineLocation find(juce::PropertySet* settings);
    static PipelineLocation at(const juce::File& dir);   // dir 可以是仓库根，也可以是 proto 本身
};

// 一次管线任务（一个子进程）。回调都在消息线程上。
class PipelineJob : private juce::Thread, private juce::AsyncUpdater {
public:
    // message：协议里的一行（step / progress / done 事件，或最后的 result / error）
    // finished：进程退出后调一次；没收到 result/error 就退出时 error 里有说明
    struct Callbacks {
        std::function<void(const juce::var& message)> message;
        std::function<void()> finished;
    };

    PipelineJob(PipelineLocation, juce::String method, juce::var params, Callbacks);
    ~PipelineJob() override;   // 还在跑就先取消，等一会儿再强杀

    bool start();              // 起不来时返回 false，原因在 startError()
    void cancel();             // 写取消标记；管线会停掉它起的子进程，再回一个 cancelled 错误
    bool isRunning() const { return isThreadRunning(); }
    bool sawReply() const { return gotReply; }
    juce::String startError() const { return startErr; }
    juce::File logFile() const { return dir.getChildFile("worker.log"); }
    juce::String logTail(int lines = 12) const;
    juce::String method() const { return meth; }

private:
    void run() override;
    void handleAsyncUpdate() override;

    PipelineLocation loc;
    juce::String meth;
    juce::var params;
    Callbacks cb;
    juce::File dir;
    juce::ChildProcess process;
    juce::CriticalSection lock;
    juce::StringArray pending;   // 读线程攒的行，消息线程取走
    bool exited = false, finishedSent = false, gotReply = false;
    juce::String startErr;
};

} // namespace reader
