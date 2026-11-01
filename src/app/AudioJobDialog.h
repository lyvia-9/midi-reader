// 打开音频后弹的对话框：选做什么（扒谱 / Suno 翻唱后扒谱）、编制预设、扒谱方式，看各个模型能不能用。
#pragma once
#include "app/AudioPipeline.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

namespace reader {

struct AudioJobRequest {
    juce::String method;   // transcribe / cover
    juce::var params;
    juce::String title;    // 进度条上显示
};

class AudioJobDialog : public juce::Component {
public:
    AudioJobDialog(juce::File audio, juce::PropertySet* settings, std::function<void(AudioJobRequest)> onStart);
    ~AudioJobDialog() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // 弹成模态窗口；点“开始”时回调 onStart 并关窗
    static void show(const juce::File& audio, juce::PropertySet* settings, std::function<void(AudioJobRequest)> onStart);

private:
    struct Preset {
        juce::String id, name, route, description, transcriber, separator;
        juce::StringArray lanes, stemTranscribers;
        bool stemsOk = false;
    };
    struct Method { juce::String label, route, transcriber; juce::StringArray needs; };

    void loadCatalog();
    void catalogArrived(const juce::var& result);
    void refresh();                      // 选项变了：重排方式列表、检查要用的模型
    void start();
    void choosePipelineDir();
    void openConfig();
    std::vector<Method> methodsFor(const Preset&) const;
    juce::String providerProblem(const juce::String& id) const;
    static juce::String providerName(const juce::String& id);
    void close();

    juce::File audio;
    juce::PropertySet* settings;
    std::function<void(AudioJobRequest)> onStart;
    PipelineLocation loc;
    std::unique_ptr<PipelineJob> catalogJob;
    std::vector<Preset> presets;
    std::map<juce::String, juce::StringArray> problems;   // provider id → 问题（空 = 能用）
    std::vector<Method> methods;
    juce::File configFile;
    bool loaded = false;

    juce::Label heading, fileLabel, whatLabel, presetLabel, presetInfo, methodLabel, styleLabel, titleLabel, status, footer;
    juce::ComboBox what, preset, method;
    juce::TextEditor style, title;
    juce::ToggleButton instrumental;
    juce::TextButton startButton, cancelButton, configButton, pipelineButton, recheckButton;
};

} // namespace reader
