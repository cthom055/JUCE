#include <juce_gui_extra/juce_gui_extra.h>
#include <csignal>
#include <dirent.h>
#include <iostream>
#include <sys/wait.h>
#include <thread>

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
        void pageFinishedLoading (const juce::String& url) override
        {
            if (! url.startsWith (getResourceProviderRoot()))
                return;
            // Keep the lifecycle test sequencing asynchronous in both dispatch modes.
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

    void closeBrowser()
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        browser.reset();
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;
        longestCloseMs = juce::jmax (longestCloseMs, elapsed);
        require (elapsed < 3000, "browser destruction takes less than three seconds");
    }

    static int descriptorCount()
    {
        auto* directory = opendir ("/proc/self/fd");
        if (directory == nullptr)
            return -1;
        int count = 0;
        while (const auto* entry = readdir (directory))
            if (entry->d_name[0] != '.')
                ++count;
        closedir (directory);
        return count - 1; // Exclude the transient directory descriptor itself.
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
        auto options = Options{}.withNativeIntegrationEnabled().withLinuxWebViewOptions (linuxOptions)
            .withEventListener ("dispatchProbe", [this] (const juce::var& event)
            {
                if (! legacyDispatch)
                    require (juce::MessageManager::getInstance()->isThisTheMessageThread(), "native events run on message thread");
                require (int (event["index"]) == probeEvents++, "burst native events stay ordered");
            })
            .withResourceProvider ([this] (const juce::String& path)
            -> std::optional<juce::WebBrowserComponent::Resource>
        {
            if (path == "/pending-resource")
            {
                pendingResourceObserved.store (true);
                const auto pid = helperPid.load();
                auto stopped = pid > 0 && kill (pid, SIGSTOP) == 0;
                if (stopped)
                {
                    // WUNTRACED observes the stop without reaping the live child.
                    int status = 0;
                    pid_t waited;
                    do { waited = waitpid (pid, &status, WUNTRACED); }
                    while (waited < 0 && errno == EINTR);
                    stopped = waited == pid && WIFSTOPPED (status);
                }
                // The helper cannot consume this resource's response before
                // destruction. Keep production transport semantics unchanged.
                juce::MessageManager::callAsync ([this, stopped]
                {
                    require (stopped, "pause helper with a URI request outstanding");
                    closeBrowser();
                    phase = 10;
                });
            }
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
            // The caller deliberately does not service the JUCE message loop
            // while this non-RT producer writes to WebKit.
            bool displaySent = false;
            std::thread displayProducer ([&]
            {
                displaySent = browser->tryEvaluateJavascriptForDisplay ("globalThis.displayProbe = 73");
            });
            displayProducer.join();
            require (displaySent, "worker sends a display frame without message-loop service");
            browser->evaluateJavascript ("globalThis.displayProbe", [this] (auto result)
            {
                require (result.getResult() != nullptr && int (*result.getResult()) == 73,
                         "worker display frame executes before the following normal evaluation");
            });
            browser->evaluateJavascript ("for(let i=0;i<256;++i) window.__JUCE__.backend.emitEvent('dispatchProbe',{index:i});");
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
                closeBrowser(); // Destruction from failure delivery must also be safe.
                phase = 5;
            });
            kill (pid, SIGKILL);
        }
        else if (phase == 6)
        {
            phase = 7;
            browser->evaluateJavascript ("1", [this] (auto result)
            {
                require (result.getResult() != nullptr && int (*result.getResult()) == 1, "repeated browser cycle result");
                closeBrowser();
                phase = 8;
            });
        }
        else if (phase == 9)
        {
            browser->evaluateJavascript ("void fetch('" + juce::WebBrowserComponent::getResourceProviderRoot()
                                         + "pending-resource')");
        }
    }

    void completedOne()
    {
        require (juce::MessageManager::getInstance()->isThisTheMessageThread(), "evaluation callback message thread");
        if (++completed != 4)
            return;
        require (probeEvents == 256, "all burst events delivered before subsequent evaluation callbacks");
        browser->evaluateJavascript ("7", [this] (auto result)
        {
            require (result.getResult() != nullptr && int (*result.getResult()) == 7, "final result");
            closeBrowser(); // This formerly destroyed/joined the active pipe reader.
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
        if (phase == 5)
        {
            phase = 6;
            openBrowser();
        }
        if (phase == 8)
        {
            ++repeatedCycles;
            const auto descriptors = descriptorCount();
            require (descriptors >= 0, "read /proc/self/fd");
            if (repeatedCycles == 3)
                warmDescriptorCount = descriptors;
            if (repeatedCycles >= 3)
                require (descriptors == warmDescriptorCount, "descriptor count stays constant after warmup");
            phase = repeatedCycles < 6 ? 6 : 9;
            openBrowser();
        }
        if (phase == 10 || failed || juce::Time::getMillisecondCounterHiRes() > deadline)
        {
            require (phase == 10, "runtime sequence completed before deadline");
            require (pendingResourceObserved.load(), "destroy browser with resource response pending");
            require (descriptorCount() == warmDescriptorCount, "pending resource cancellation releases descriptors");
            setApplicationReturnValue (failed ? 1 : 0);
            if (! failed)
                std::cout << "Linux WebView runtime passed: mixed callbacks, callback destruction, helper death, six reopen cycles, "
                          << "and pending resource cancellation; descriptors=" << warmDescriptorCount
                          << "; maximum close=" << longestCloseMs << "ms; runtime="
                          << juce::Time::getMillisecondCounterHiRes() - (deadline - 30000) << "ms\n";
            quit();
        }
    }

    std::unique_ptr<Browser> browser;
    std::atomic<int> helperPid { 0 };
    std::atomic<bool> pendingResourceObserved { false };
    const bool legacyDispatch = juce::SystemStats::getEnvironmentVariable ("JUCE_WEBVIEW_LEGACY_DISPATCH", "0") == "1";
    int probeEvents = 0;
    int phase = 0, completed = 0;
    int repeatedCycles = 0, warmDescriptorCount = -1;
    bool failed = false;
    double deadline = 0, longestCloseMs = 0;
};

START_JUCE_APPLICATION (LinuxWebViewTests)
