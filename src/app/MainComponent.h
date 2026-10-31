#pragma once
#include "app/Document.h"
#include "app/LaneDetailView.h"
#include "app/MultiLaneCanvas.h"
#include "app/SidePanel.h"
#include "app/Transport.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace reader {

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void openFile(const juce::File&);
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragOver = false; repaint(); }
    void filesDropped(const juce::StringArray&, int, int) override;

    std::function<void(const juce::String&)> onTitleChanged;

private:
    void timerCallback() override;
    void documentChanged();
    void showMessage(const juce::String&);
    void chooseFile();
    void openDetail(int lane);
    void showNoteMenu(int lane, const LaneNote&);
    void updateTransportButtons();

    Document doc;
    Transport transport;
    MultiLaneCanvas canvas{doc.laneModel()};
    SidePanel side{doc};
    juce::StretchableLayoutManager layoutManager;
    juce::StretchableLayoutResizerBar resizer{&layoutManager, 1, true};

    juce::TextButton openButton, playButton, stopButton, fitButton, reanalyzeButton;
    juce::ToggleButton followToggle, ksToggle;
    juce::ComboBox pitchFit;
    juce::Label clock, message;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<std::unique_ptr<LaneDetailWindow>> details;
    juce::ApplicationProperties props;
    juce::TooltipWindow tooltips{this, 600};
    bool dragOver = false;
    int messageTicks = 0;
    double lastClockTick = -1;
};

} // namespace reader
