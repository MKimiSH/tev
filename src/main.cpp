// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/ImageViewer.h"
#include "../include/SharedQueue.h"
#include "../include/ThreadPool.h"

#include <args.hxx>
#include <ImfThreading.h>

#include <iostream>
#include <thread>

#define NOMINMAX
#include <Windows.h>
#include <io.h>
#include <fcntl.h>

using namespace args;
using namespace std;

TEV_NAMESPACE_BEGIN

int mainFunc(int argc, char* argv[]) {
    auto ipc = make_shared<Ipc>();

    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    ArgumentParser parser{
        "Inspection tool for images with a high dynamic range.",
        "",
    };

    HelpFlag helpFlag{
        parser,
        "help",
        "Display this help menu",
        {'h', "help"},
    };

    ValueFlag<float> exposureFlag{
        parser,
        "exposure",
        "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure. "
        "It can be controlled via the GUI, or by pressing E/Shift+E.",
        {'e', "exposure"},
    };

    ValueFlag<string> filterFlag{
        parser,
        "filter",
        "Filters visible images and layers according to a supplied string. "
        "The string must have the format 'image:layer'. "
        "Only images whose name contains 'image' and layers whose name contains 'layer' will be visible.",
        {'f', "filter"},
    };

    ValueFlag<bool> maximizeFlag{
        parser,
        "maximize",
        "Whether to maximize the window on startup or not. "
        "If no images were supplied via the command line, then the default is false. "
        "Otherwise, the default is true.",
        {"max", "maximize"},
    };

    ValueFlag<string> metricFlag{
        parser,
        "metric",
        "The metric to use when comparing two images. "
        R"(
        The available metrics are:
        E   - Error
        AE  - Absolute Error
        SE  - Squared Error
        RAE - Relative Absolute Error
        RSE - Relative Squared Error
        )"
        "Default is E.",
        {'m', "metric"},
    };

    ValueFlag<float> offsetFlag{
        parser,
        "offset",
        "The offset is added to the image after exposure has been applied. "
        "It can be controlled via the GUI, or by pressing O/Shift+O.",
        {'o', "offset"},
    };

    ValueFlag<string> tonemapFlag{
        parser,
        "tonemap",
        "The tonemapping algorithm to use. "
        R"(
        The available tonemaps are:
        sRGB   - sRGB
        Gamma  - Gamma curve (2.2)
        FC     - False Color
        PN     - Positive=Green, Negative=Red
        )"
        "Default is sRGB.",
        {'t', "tonemap"},
    };

    PositionalList<string> imageFiles{
        parser,
        "images or channel selectors",
        "The image files to be opened by the viewer. "
        "If a filename starting with a ':' is encountered, "
        "then this filename is not treated as an image file "
        "but as a comma-separated channel selector. Until the next channel "
        "selector is encountered only channels containing "
        "elements from the current selector will be loaded. This is "
        "especially useful for selectively loading a specific "
        "part of a multi-part EXR file.",
    };

    // Parse command line arguments and react to parsing
    // errors using exceptions.
    try {
        parser.ParseCLI(argc, argv);
    } catch (args::Help) {
        std::cout << parser;
        return 0;
    } catch (args::ParseError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return -1;
    } catch (args::ValidationError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return -2;
    }

    // If we're not the primary instance, simply send the to-be-opened images
    // to the primary instance.
    if (!ipc->isPrimaryInstance()) {
        string channelSelector;
        for (auto imageFile : get(imageFiles)) {
            if (!imageFile.empty() && imageFile[0] == ':') {
                channelSelector = imageFile.substr(1);
                continue;
            }

            try {
                ipc->sendToPrimaryInstance(absolutePath(imageFile) + ":" + channelSelector);
            } catch (runtime_error e) {
                cerr << "Invalid file '" << imageFile << "': " << e.what() << endl;
            }
        }

        return 0;
    }

    cout << "Loading window..." << endl;

    // Load images passed via command line in the background prior to
    // creating our main application such that they are not stalled
    // by the potentially slow initialization of opengl / glfw.
    shared_ptr<SharedQueue<ImageAddition>> imagesToAdd = make_shared<SharedQueue<ImageAddition>>();
    string channelSelector;
    for (auto imageFile : get(imageFiles)) {
        if (!imageFile.empty() && imageFile[0] == ':') {
            channelSelector = imageFile.substr(1);
            continue;
        }

        ThreadPool::singleWorker().enqueueTask([imageFile, channelSelector, &imagesToAdd] {
            auto image = tryLoadImage(imageFile, channelSelector);
            if (image) {
                imagesToAdd->push({false, image});
            }
        });
    }

    // Init nanogui application
    nanogui::init();

    {
        auto app = unique_ptr<ImageViewer>{new ImageViewer{ipc, imagesToAdd}};
        app->drawAll();
        app->setVisible(true);

        // Do what the maximize flag tells us---if it exists---and
        // maximize if we have images otherwise.
        if (maximizeFlag ? get(maximizeFlag) : imageFiles) {
            app->maximize();
        }

        // Apply parameter flags
        if (exposureFlag) { app->setExposure(get(exposureFlag)); }
        if (filterFlag)   { app->setFilter(get(filterFlag)); }
        if (metricFlag)   { app->setMetric(toMetric(get(metricFlag))); }
        if (offsetFlag)   { app->setOffset(get(offsetFlag)); }
        if (tonemapFlag)  { app->setTonemap(toTonemap(get(tonemapFlag))); }

        // Refresh only every 250ms if there are no user interactions.
        // This makes an idling tev surprisingly energy-efficient. :)
        nanogui::mainloop(250);
    }

    // On some linux distributions glfwTerminate() (which is called by
    // nanogui::shutdown()) causes segfaults. Since we are done with our
    // program here anyways, let's let the OS clean up after us.
    //nanogui::shutdown();

    // Let all threads gracefully terminate.
    ThreadPool::shutdown();

    return 0;
}

TEV_NAMESPACE_END

int main(int argc, char* argv[]) {
    try {
        tev::mainFunc(argc, argv);
    } catch (const runtime_error& e) {
        cerr << "Uncaught exception: " << e.what() << endl;
        return 1;
    }
}

#ifdef _WIN32
bool reconnectIo(bool openNewConsole) {
    bool madeConsole = false;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!openNewConsole)
            return false;

        madeConsole = true;
        if (!AllocConsole())
            return false;
    }

    freopen("CON", "w", stdout);
    freopen("CON", "w", stderr);
    freopen("CON", "r", stdin);

    // C++ streams to console
    ios::sync_with_stdio();

    return madeConsole;
}

int APIENTRY WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    reconnectIo(false);
    return main(__argc, __argv);
}
#endif
