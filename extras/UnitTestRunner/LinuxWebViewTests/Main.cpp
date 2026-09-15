#include <juce_gui_extra/juce_gui_extra.h>
#include <csignal>
#include <iostream>

class LinuxWebViewTests final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "Linux WebView Tests"; }
    const juce::String getApplicationVersion() override { return "1.0"; }

    void initialise (const juce::String& arguments) override
    {
        juce::UnitTestRunner runner;
        runner.setAssertOnFailure (false);
        runner.runTestsInCategory ("Linux WebView");
        for (int i = 0; i < runner.getNumResults(); ++i)
            if (runner.getResult (i)->failures != 0)
                failed = true;
        if (failed || arguments.contains ("--protocol-only"))
        {
            setApplicationReturnValue (failed ? 1 : 0);
            quit();
            return;
        }
        deadline = juce::Time::getMillisecondCounterHiRes() + 30000;
        openBrowser();
        startTimer (50);
    }

    void shutdown() override { stopTimer(); browser.reset(); }

private:
    class Browser final : public juce::WebBrowserComponent
    {
    public:
        Browser (const Options& options, std::function<void()> readyIn)
            : WebBrowserComponent (options), ready (std::move (readyIn)) {}
        void pageFinishedLoading (const juce::String&) override
        {
            // Linux command dispatch deliberately still uses the reader/MML.
            juce::MessageManager::callAsync (ready);
        }
    private:
        std::function<void()> ready;
    };

    void require (bool condition, const char* message)
    {
        if (! condition)
        {
            failed = true;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    void openBrowser()
    {
        helperPid.store (0);
        using Options = juce::WebBrowserComponent::Options;
        using Linux = Options::LinuxWebView;
        auto linuxOptions = Linux{}.withHardwareAccelerationPolicy (Linux::HardwareAccelerationPolicy::never)
                            .withRendererProfile (Linux::RendererProfile::disableCompositing)
                            .withDiagnosticLogCallback ([this] (const juce::String& message)
                            {
                                std::cerr << message << '\n';
                                if (message.contains ("child entry pid="))
                                    helperPid.store (message.fromFirstOccurrenceOf ("child entry pid=", false, false).getIntValue());
                            });
        auto options = Options{}.withLinuxWebViewOptions (linuxOptions).withResourceProvider ([] (const juce::String&)
            -> std::optional<juce::WebBrowserComponent::Resource>
        {
            juce::Thread::sleep (25); // Complete the retained URI request asynchronously.
            const std::string html = "<!doctype html><html><body>WebView stability test</body></html>";
            return juce::WebBrowserComponent::Resource { std::vector<std::byte> (
                reinterpret_cast<const std::byte*> (html.data()),
                reinterpret_cast<const std::byte*> (html.data() + html.size())), "text/html" };
        });
        browser = std::make_unique<Browser> (options, [this] { browserReady(); });
        browser->setBounds (0, 0, 400, 300);
        browser->addToDesktop (0);
        browser->setVisible (true);
        browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    }

    void browserReady()
    {
        if (phase == 0)
        {
            phase = 1;
            browser->evaluateJavascript ("globalThis.marker = 10");
            browser->evaluateJavascript ("41 + 1", [this] (auto result)
            {
                require (result.getResult() != nullptr && int (*result.getResult()) == 42, "mixed callback result 42");
                completedOne();
            });
            browser->evaluateJavascript ("globalThis.marker = 20");
            browser->evaluateJavascript ("globalThis.marker", [this] (auto result)
            {
                require (result.getResult() != nullptr && int (*result.getResult()) == 20, "mixed callback result 20");
                completedOne();
            });
            browser->evaluateJavascript ("throw new Error('expected')", [this] (auto result)
            {
                require (result.getError() != nullptr, "JavaScript exception callback");
                completedOne();
            });
            browser->evaluateJavascript ("undefined", [this] (auto result)
            {
                require (result.getResult() != nullptr && result.getResult()->isUndefined(), "undefined callback");
                completedOne();
            });
        }
        else if (phase == 3)
        {
            phase = 4;
            const auto pid = helperPid.load();
            require (pid > 0, "helper PID observed");
            if (pid <= 0)
                return;
            kill (pid, SIGSTOP);
            browser->evaluateJavascript ("42", [this] (auto result)
            {
                require (juce::MessageManager::getInstance()->isThisTheMessageThread(), "failure callback message thread");
                require (result.getError() != nullptr, "pending callback fails on helper death");
                browser.reset(); // Destruction from failure delivery must also be safe.
                phase = 5;
            });
            kill (pid, SIGKILL);
        }
    }

    void completedOne()
    {
        require (juce::MessageManager::getInstance()->isThisTheMessageThread(), "evaluation callback message thread");
        if (++completed != 4)
            return;
        browser->evaluateJavascript ("7", [this] (auto result)
        {
            require (result.getResult() != nullptr && int (*result.getResult()) == 7, "final result");
            browser.reset(); // This formerly destroyed/joined the active pipe reader.
            phase = 2;
        });
    }

    void timerCallback() override
    {
        if (phase == 2)
        {
            phase = 3;
            openBrowser();
        }
        if (phase == 5 || failed || juce::Time::getMillisecondCounterHiRes() > deadline)
        {
            require (phase == 5, "runtime sequence completed before deadline");
            setApplicationReturnValue (failed ? 1 : 0);
            if (! failed)
                std::cout << "Linux WebView runtime: mixed callbacks, callback destruction, reopen, and helper death passed\n";
            quit();
        }
    }

    std::unique_ptr<Browser> browser;
    std::atomic<int> helperPid { 0 };
    int phase = 0, completed = 0;
    bool failed = false;
    double deadline = 0;
};

START_JUCE_APPLICATION (LinuxWebViewTests)
