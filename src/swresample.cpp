#include <nan.h>

NAN_METHOD(create) {
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, create);
}

NODE_MODULE(swresample, Initialize)
