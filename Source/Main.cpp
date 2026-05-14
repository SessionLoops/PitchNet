// Main.cpp - Cross-platform entry point (macOS uses native menu inside
// MainComponent)

#include "JuceHeader.h"
#include "UI/MainComponent.h"
#include "UI/StyledComponents.h"
#include "Utils/AppLogger.h"
#include "Utils/Constants.h"
#include "Utils/Localization.h"
#include "Utils/PlatformUtils.h"
#include "Utils/UI/WindowSizing.h"
#include "Utils/UI/TimecodeFont.h"

#if JUCE_WINDOWS
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

class SplashComponent : public juce::Component, private juce::Timer {
public:
  SplashComponent() { startTimerHz(30); }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient background(
        juce::Colour(APP_COLOR_BACKGROUND).brighter(0.12f),
        bounds.getTopLeft(),
        juce::Colour(APP_COLOR_BACKGROUND).darker(0.12f),
        bounds.getBottomRight(),
        false);
    g.setGradientFill(background);
    g.fillAll();

    const auto titleFont = AppFont::getBoldFont(34.0f);
    const auto subtitleFont = AppFont::getFont(15.0f);

    g.setColour(juce::Colours::white);
    g.setFont(titleFont);
    g.drawText("PitchNet", getLocalBounds().reduced(24, 20),
               juce::Justification::centredTop);

    g.setColour(juce::Colour(APP_COLOR_PRIMARY));
    g.setFont(subtitleFont);
    g.drawText(TR("progress.loading"), 0, 150, getWidth(), 24,
               juce::Justification::centredTop);

    const float dotRadius = 5.0f;
    const float dotSpacing = 14.0f;
    const float baseY = 190.0f;
    const float startX = (getWidth() - dotSpacing * 2.0f) * 0.5f;

    for (int i = 0; i < 3; ++i) {
      const int phase = (tick / 6 + i) % 3;
      const float alpha = (phase == 0 ? 1.0f : (phase == 1 ? 0.6f : 0.35f));
      g.setColour(juce::Colour(APP_COLOR_PRIMARY).withAlpha(alpha));
      g.fillEllipse(startX + dotSpacing * static_cast<float>(i),
                    baseY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
    }
  }

private:
  void timerCallback() override {
    ++tick;
    repaint();
  }

  int tick = 0;
};

class SplashWindow : public juce::DocumentWindow {
public:
  SplashWindow()
      : juce::DocumentWindow("",
                             juce::Colour(APP_COLOR_BACKGROUND),
                             juce::DocumentWindow::closeButton,
                             false) {
    setUsingNativeTitleBar(false);
    setTitleBarButtonsRequired(0, false);
    setResizable(false, false);
    setAlwaysOnTop(true);
    setOpaque(true);

    auto *content = new SplashComponent();
    setContentOwned(content, true);
    setSize(420, 230);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
  }

  void closeButtonPressed() override {}
};

class PitchNetApplication : public juce::JUCEApplication {
public:
  PitchNetApplication() {}

  const juce::String getApplicationName() override { return "PitchNet"; }

  const juce::String getApplicationVersion() override { return "1.0.0"; }

  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String &commandLine) override {
    juce::ignoreUnused(commandLine);
    AppLogger::init();
    LOG("========== APP STARTING ==========");
    LOG("Initializing fonts...");
    AppFont::initialize();
    TimecodeFont::initialize();
    LOG("Loading localization...");
    Localization::loadFromSettings();
    LOG("Localization loaded, showing splash...");
#if JUCE_STANDALONE_APPLICATION
    splashWindow = std::make_unique<SplashWindow>();
#endif
    juce::MessageManager::callAsync([this]() {
      LOG("Creating MainWindow...");
      mainWindow = std::make_unique<MainWindow>(getApplicationName());
      splashWindow = nullptr;
      LOG("MainWindow created and visible");
    });
  }

  void shutdown() override {
    mainWindow = nullptr;
    TimecodeFont::shutdown();
    AppFont::shutdown(); // Release font resources before JUCE shuts down
  }

  void systemRequestedQuit() override { quit(); }

  void anotherInstanceStarted(const juce::String &commandLine) override {
    juce::ignoreUnused(commandLine);
  }

  class MainWindow : public juce::DocumentWindow {
  public:
    MainWindow(juce::String name)
        : DocumentWindow(name, juce::Colour(APP_COLOR_BACKGROUND),
                         DocumentWindow::allButtons,
                         false) // Don't add to desktop yet
    {
      LOG("MainWindow: constructor start");

      // Ensure window is opaque - this must be set before any
      // transparency-related operations
      setOpaque(true);

      LOG("MainWindow: creating MainComponent...");
      // Set content first, ensuring it's also opaque
      auto *content = new MainComponent();
      LOG("MainWindow: MainComponent created");
      content->setOpaque(true);
      setContentOwned(content, true);

      // Now set native title bar after content is set
      setUsingNativeTitleBar(true);

      setResizable(true, true);

      // Ensure window is still opaque before adding to desktop
      // (some operations might affect opacity state)
      setOpaque(true);

      LOG("MainWindow: adding to desktop...");
      // Now add to desktop after all properties are set
      addToDesktop();

      auto *display = WindowSizing::getDisplayForComponent(this);
      auto constraints = WindowSizing::Constraints();
      auto desiredSize = content->getSavedWindowSize();
      if (desiredSize.x <= 0 || desiredSize.y <= 0)
        desiredSize = {WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight};

      if (display != nullptr) {
        auto initialBounds = WindowSizing::getInitialBounds(
            desiredSize.x, desiredSize.y, *display, constraints);
        auto maxBounds = WindowSizing::getMaxBounds(*display);
        setBounds(initialBounds);
        setResizeLimits(constraints.minWidth, constraints.minHeight,
                        maxBounds.getWidth(), maxBounds.getHeight());
      } else {
        setSize(desiredSize.x, desiredSize.y);
        centreWithSize(getWidth(), getHeight());
      }

      LOG("MainWindow: initial size " + juce::String(getWidth()) + "x" +
          juce::String(getHeight()));
      setVisible(true);
      LOG("MainWindow: setVisible(true) done");

#if JUCE_WINDOWS
      // Enable dark mode for title bar
      if (auto *peer = getPeer()) {
        if (auto hwnd = (HWND)peer->getNativeHandle()) {
          // Enable immersive dark mode for title bar
          constexpr DWORD darkMode = 1;
          DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                                &darkMode, sizeof(darkMode));

          // Enable rounded corners on Windows 11+
          DWORD preference = 2; // DWMWCP_ROUND
          DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                                &preference, sizeof(preference));
        }
      }
#elif JUCE_MAC
      // Enable dark mode for macOS window
      if (auto *peer = getPeer())
        PlatformUtils::setDarkAppearance(peer->getNativeHandle());
#endif
    }

    void closeButtonPressed() override {
      JUCEApplication::getInstance()->systemRequestedQuit();
    }

  private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
  };

private:
  std::unique_ptr<MainWindow> mainWindow;
#if JUCE_STANDALONE_APPLICATION
  std::unique_ptr<SplashWindow> splashWindow;
#endif
};

START_JUCE_APPLICATION(PitchNetApplication)
