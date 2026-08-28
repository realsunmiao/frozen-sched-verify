#include "RdPredictor.h"
#include <android-base/logging.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>
#include <tensorflow/lite/micro/micro_error_reporter.h>
#include <cstring>

using namespace tflite;

RdPredictor& RdPredictor::getInstance() {
    static RdPredictor instance;
    return instance;
}

bool RdPredictor::loadModel(const std::string& model_path) {
    // Load TFLite model from file
    FILE* f = fopen(model_path.c_str(), "rb");
    if (!f) {
        LOG(ERROR) << "Failed to open model: " << model_path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> buffer(size);
    fread(buffer.data(), 1, size, f);
    fclose(f);

    const Model* model = GetModel(buffer.data());
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        LOG(ERROR) << "Model schema version mismatch";
        return false;
    }

    static MicroMutableOpResolver<4> resolver;
    resolver.AddFullyConnected();
    resolver.AddRelu();
    resolver.AddReshape();
    resolver.AddSoftmax();

    static MicroErrorReporter error_reporter;
    static MicroInterpreter interpreter(model, resolver, nullptr, 0, &error_reporter);
    if (interpreter.AllocateTensors() != kTfLiteOk) {
        LOG(ERROR) << "AllocateTensors failed";
        return false;
    }

    interpreter_ = &interpreter;
    loaded_ = true;
    LOG(INFO) << "RD predictor loaded: " << model_path;
    return true;
}

float RdPredictor::predict(const float features[8]) const {
    if (!loaded_) return -1.0f;
    auto* interpreter = static_cast<MicroInterpreter*>(interpreter_);
    TfLiteTensor* input = interpreter->input(0);
    TfLiteTensor* output = interpreter->output(0);
    if (input->bytes != 8 * sizeof(float) || output->bytes != sizeof(float)) {
        return -1.0f;
    }
    memcpy(input->data.f, features, 8 * sizeof(float));
    if (interpreter->Invoke() != kTfLiteOk) {
        return -1.0f;
    }
    return output->data.f[0];
}

void RdPredictor::updateModel(const std::vector<uint8_t>& grad) {
    // v1.1: federated learning - apply gradient to local model
    LOG(INFO) << "RD predictor model update, grad size=" << grad.size();
    // TODO: implement gradient application (requires mutable tensor access)
}