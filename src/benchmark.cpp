#include "cmd_line_parser.h"
#include "engine.h"
#include <chrono>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/opencv.hpp>

int main(int argc, char *argv[]) {
    CommandLineArguments arguments;

    // Parse the command line arguments
    if (!parseArguments(argc, argv, arguments)) {
        return -1;
    }

    // Specify our GPU inference configuration options
    Options options;
    // Specify what precision to use for inference
    // FP16 is approximately twice as fast as FP32.
    options.precision = Precision::FP16;
    // If using INT8 precision, must specify path to directory containing
    // calibration data.
    options.calibrationDataDirectoryPath = "";
    // Specify the batch size to optimize for.
    options.optBatchSize = 1;
    // Specify the maximum batch size we plan on running.
    options.maxBatchSize = 1;
    // Specify the directory where you want the model engine model file saved.
    options.engineFileDir = ".";

    Engine<float> engine(options);

    // Define our preprocessing code
    // The default Engine::build method will normalize values between [0.f, 1.f]
    // Setting the normalize flag to false will leave values between [0.f,
    // 255.f] (some converted models may require this).

    // For our YoloV8 model, we need the values to be normalized between
    // [0.f, 1.f] so we use the following params
    std::array<float, 3> subVals{0.f, 0.f, 0.f};
    std::array<float, 3> divVals{1.f, 1.f, 1.f};
    bool normalize = true;
    // Note, we could have also used the default values.

    // If the model requires values to be normalized between [-1.f, 1.f], use
    // the following params:
    //    subVals = {0.5f, 0.5f, 0.5f};
    //    divVals = {0.5f, 0.5f, 0.5f};
    //    normalize = true;

    if (!arguments.onnxModelPath.empty()) {
        // Build the onnx model into a TensorRT engine file, and load the
        // TensorRT engine file into memory.
        bool succ = engine.buildLoadNetwork(arguments.onnxModelPath, subVals,
                                            divVals, normalize);
        if (!succ) {
            throw std::runtime_error(
                "Unable to build or load TensorRT engine.");
        }
    } else {
        // Load the TensorRT engine file directly
        bool succ = engine.loadNetwork(arguments.trtModelPath, subVals, divVals,
                                       normalize);
        if (!succ) {
            const std::string msg = "Unable to load TensorRT engine.";
            std::cerr << msg << '\n';
            throw std::runtime_error(msg);
        }
    }

    // Read the input image
    // TODO: You will need to read the input image required for your model
    const std::string inputImage = arguments.imagePath;
    auto cpuImg = cv::imread(inputImage);
    if (cpuImg.empty()) {
        const std::string msg = "Unable to read image at path: " + inputImage;
        std::cerr << msg << '\n';
        throw std::runtime_error(msg);
    }

    // Upload the image GPU memory
    cv::cuda::GpuMat img;
    img.upload(cpuImg);

    // The model expects RGB input
    cv::cuda::cvtColor(img, img, cv::COLOR_BGR2RGB);

    // In the following section we populate the input vectors to later pass for
    // inference
    const auto &inputDims = engine.getInputDims();
    std::vector<std::vector<cv::cuda::GpuMat>> inputs;

    // Let's use a batch size which matches that which we set the
    // Options.optBatchSize option
    size_t batchSize = options.optBatchSize;

    // TODO:
    // For the sake of the demo, we will be feeding the same image to all the
    // inputs You should populate your inputs appropriately.
    for (const auto &inputDim : inputDims) { // For each of the model inputs...
        std::vector<cv::cuda::GpuMat> input;
        for (size_t j = 0; j < batchSize;
             ++j) { // For each element we want to add to the batch...
            // TODO:
            // You can choose to resize by scaling, adding padding, or a
            // combination of the two in order to maintain the aspect ratio You
            // can use the Engine::resizeKeepAspectRatioPadRightBottom to resize
            // to a square while maintain the aspect ratio (adds padding where
            // necessary to achieve this).
            auto resized = Engine<float>::resizeKeepAspectRatioPadRightBottom(
                img, inputDim.d[1], inputDim.d[2]);
            // You could also perform a resize operation without maintaining
            // aspect ratio with the use of padding by using the following
            // instead:
            //            cv::cuda::resize(img, resized, cv::Size(inputDim.d[2],
            //            inputDim.d[1])); // TRT dims are (height, width)
            //            whereas OpenCV is (width, height)
            input.emplace_back(std::move(resized));
        }
        inputs.emplace_back(std::move(input));
    }

    // Warm up the network before we begin the benchmark
    std::cout << "Warming up the network...\n";
    std::vector<std::vector<std::vector<float>>> featureVectors;
    for (int i = 0; i < 100; ++i) {
        bool succ = engine.runInference(inputs, featureVectors);
        if (!succ) {
            const std::string msg = "Unable to run inference.";
            std::cerr << msg << '\n';
            throw std::runtime_error(msg);
        }
    }

    // Benchmark the inference time
    size_t numIterations = 1000;
    std::cout << "Running benchmarks (" << numIterations << " iterations)...\n";
    preciseStopwatch stopwatch;
    for (size_t i = 0; i < numIterations; ++i) {
        featureVectors.clear();
        engine.runInference(inputs, featureVectors);
    }
    auto totalElapsedTimeMs =
        stopwatch.elapsedTime<float, std::chrono::milliseconds>();
    auto avgElapsedTimeMs = totalElapsedTimeMs / numIterations /
                            static_cast<float>(inputs[0].size());

    std::cerr << "Benchmarking complete!\n"
              << "======================\n"
              << "Avg time per sample: " << avgElapsedTimeMs << " ms\n"
              << "Batch size: " << inputs.size() << "\n"
              << "Avg FPS: " << static_cast<int>(1000 / avgElapsedTimeMs)
              << " fps\n"
              << "======================\n";

    // Print the feature vectors
    for (size_t batch = 0; batch < featureVectors.size(); ++batch) {
        for (size_t outputNum = 0; outputNum < featureVectors[batch].size();
             ++outputNum) {
            std::cerr << "Batch " << batch << ", output " << outputNum << '\n';
            std::string output;
            int i = 0;
            for (const auto &e : featureVectors[batch][outputNum]) {
                output += std::to_string(e) + " ";
                if (++i == 10) {
                    output += "...";
                    break;
                }
            }
            std::cout << output << '\n';
        }
    }

    // TODO: If your model requires post processing (ex. convert feature vector
    // into bounding boxes) then you would do so here.

    return 0;
}