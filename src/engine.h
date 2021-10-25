#pragma once

#include <opencv2/opencv.hpp>

#include "NvInfer.h"
#include "buffers.h"

// Options for the network
struct Options {
    // Use 16 byte floating point type for inference
    bool FP16 = false;
    // Batch size to optimize for
    uint32_t optBatchSize = 1;
    // Maximum allowable batch size
    uint32_t maxBatchSize = 1;
    // Max allowable GPU memory to be used for model conversion, in bytes
    size_t maxWorkspaceSize = 4000000000;
};

// Class to extend TensorRT logger
class Logger : public nvinfer1::ILogger {
    void log (Severity severity, const char* msg) noexcept override;
};

class Engine {
public:
    // Build the network
    bool build(std::string onnxModelPath);
    // Load and prepare the network for inference
    bool loadNetwork();
    // Run inference
    bool runInference(const std::vector<cv::Mat>& inputFaceChips, std::vector<float> featureVector);
private:
    // Converts the engine options into a string
    std::string serializeEngineOptions();

    bool doesFileExist(const std::string& filepath);


    std::shared_ptr<nvinfer1::ICudaEngine> m_engine = nullptr;
    Logger m_logger;
    nvinfer1::Dims m_inputDims;
    nvinfer1::Dims m_outputDims;
    samplesCommon::ManagedBuffer m_inputBuff;
    samplesCommon::ManagedBuffer m_ouputBuff;
    size_t m_prevBatchSize = 0;
};