#pragma once

#include <vector>
#include <string>

class RdPredictor {
public:
    static RdPredictor& getInstance();
    bool loadModel(const std::string& model_path);
    float predict(const float features[8]) const; // returns predicted RD in seconds
    void updateModel(const std::vector<uint8_t>& grad); // v1.1

private:
    RdPredictor() = default;
    void* interpreter_ = nullptr; // TFLite Micro interpreter
    bool loaded_ = false;
};