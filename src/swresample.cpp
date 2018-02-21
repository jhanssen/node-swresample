#include <nan.h>
#include <unordered_map>
#include "utils.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

struct Resample : public Nan::ObjectWrap
{
    Resample();
    ~Resample();

    uv_thread_t thread;
    uv_async_t async;

    std::unordered_map<std::string, std::vector<std::shared_ptr<Nan::Callback> > > ons;

    struct Format {
        int64_t channels;
        int32_t rate;
        AVSampleFormat format;
    };

    struct Data {
        enum Type { Samples, Error, Stop, SrcFmt, DstFmt, End } type;

        uint8_t* data;
        size_t size;

        Format fmt;
    };
    WaitQueue<Data> input;
    Queue<Data> output;

    v8::Local<v8::Object> makeObject();
    void open();
    void throwError(const char* msg);
    static void run(void* arg);
};

Resample::Resample()
{
    async.data = this;
}

Resample::~Resample()
{
    input.push(Data{ Data::Stop, nullptr, 0, {} });
    uv_thread_join(&thread);
    uv_close(reinterpret_cast<uv_handle_t*>(&async), nullptr);
}

void Resample::open()
{
    uv_async_init(uv_default_loop(), &async, [](uv_async_t* async) {
            Resample* r = static_cast<Resample*>(async->data);
            bool ok;
            for (;;) {
                Data data = r->output.pop(&ok);
                if (!ok)
                    break;
                switch (data.type) {
                case Data::Error: {
                    Nan::HandleScope scope;
                    auto strerror = Nan::New<v8::String>(reinterpret_cast<char*>(data.data), data.size).ToLocalChecked();
                    auto error = Nan::Error(strerror);

                    const auto& o = r->ons["error"];
                    for (const auto& cb : o) {
                        if (!cb->IsEmpty()) {
                            cb->Call(1, &error);
                        }
                    }

                    free(data.data);
                    break; }
                case Data::Samples: {
                    // don't free data here, buffer takes ownership
                    Nan::HandleScope scope;
                    v8::Local<v8::Value> value = Nan::NewBuffer(reinterpret_cast<char*>(data.data), data.size).ToLocalChecked();

                    const auto& o = r->ons["samples"];
                    for (const auto& cb : o) {
                        if (!cb->IsEmpty()) {
                            cb->Call(1, &value);
                        }
                    }

                    break; }
                case Data::End: {
                    Nan::HandleScope scope;
                    const auto& o = r->ons["end"];
                    for (const auto& cb : o) {
                        if (!cb->IsEmpty()) {
                            cb->Call(0, nullptr);
                        }
                    }

                    break; }
                case Data::SrcFmt:
                case Data::DstFmt:
                case Data::Stop:
                    // won't happen
                    break;
                }
            }
        });
    uv_thread_create(&thread, run, this);
}

void Resample::throwError(const char* msg)
{
    output.push(Data{ Data::Error, reinterpret_cast<uint8_t*>(strdup(msg)), strlen(msg), {} });
    uv_async_send(&async);
}

static int bitsPerSample(AVSampleFormat fmt)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_U8:
        return 8;
    case AV_SAMPLE_FMT_S16:
        return 16;
    case AV_SAMPLE_FMT_S32:
        return 32;
    case AV_SAMPLE_FMT_FLT:
        return 32;
    case AV_SAMPLE_FMT_DBL:
        return 64;
    default:
        break;
    }
    return 0;
}

void Resample::run(void* arg)
{
    Resample* r = static_cast<Resample*>(arg);
    struct {
        SwrContext* swr;
        uint8_t** dstdata;
        int srcchannels, dstchannels;
        int maxdstsamples;
        int dstline;
    } state{};

    Format srcfmt{};
    Format dstfmt{};

    auto recreate = [&srcfmt, &dstfmt, &state, r](int srcsamples) -> bool {
        if (!srcfmt.rate || !dstfmt.rate) {
            // let's not
            return false;
        }
        // printf("src %ld %d %d\n", srcfmt.channels, srcfmt.rate, srcfmt.format);
        // printf("dst %ld %d %d\n", dstfmt.channels, dstfmt.rate, dstfmt.format);
        if (state.swr) {
            swr_free(&state.swr);
            av_freep(state.dstdata[0]);
            av_freep(state.dstdata);
        }
        state.swr = swr_alloc();
        av_opt_set_int(state.swr, "in_channel_layout", srcfmt.channels, 0);
        av_opt_set_int(state.swr, "in_sample_rate", srcfmt.rate, 0);
        av_opt_set_sample_fmt(state.swr, "in_sample_fmt", srcfmt.format, 0);

        av_opt_set_int(state.swr, "out_channel_layout", dstfmt.channels, 0);
        av_opt_set_int(state.swr, "out_sample_rate", dstfmt.rate, 0);
        av_opt_set_sample_fmt(state.swr, "out_sample_fmt", dstfmt.format, 0);

        int ret = swr_init(state.swr);
        if (ret < 0) {
            // error
            r->throwError("Unable to initialize state.swr");
            swr_free(&state.swr);
            state.swr = nullptr;
            return false;
        }

        state.srcchannels = av_get_channel_layout_nb_channels(srcfmt.channels);
        // ret = av_samples_alloc_array_and_samples(&state.srcdata, &state.srcline, state.srcchannels,

        state.maxdstsamples = av_rescale_rnd(srcsamples, dstfmt.rate, srcfmt.rate, AV_ROUND_UP);

        state.dstchannels = av_get_channel_layout_nb_channels(dstfmt.channels);
        ret = av_samples_alloc_array_and_samples(&state.dstdata, &state.dstline, state.dstchannels,
                                                 state.maxdstsamples, dstfmt.format, 0);
        if (ret < 0) {
            r->throwError("Unable to allocate dst sample array");
            swr_free(&state.swr);
            state.swr = nullptr;
            return false;
        }

        return true;
    };

    auto reset = [&state]() {
        if (state.swr) {
            swr_free(&state.swr);
            av_freep(state.dstdata[0]);
            av_freep(state.dstdata);
            state.swr = nullptr;
        }
    };

    int lastsrcsamples = 1024;
    for (;;) {
        Data data = r->input.wait();
        switch (data.type) {
        case Data::Stop:
        case Data::Error:
            reset();
            return;
        case Data::SrcFmt:
            if (!bitsPerSample(data.fmt.format)) {
                r->throwError("Unable to set source format");
                break;
            }
            srcfmt = data.fmt;
            recreate(lastsrcsamples);
            break;
        case Data::DstFmt:
            if (!bitsPerSample(data.fmt.format)) {
                r->throwError("Unable to set destination format");
                break;
            }
            dstfmt = data.fmt;
            recreate(lastsrcsamples);
            break;
        case Data::Samples: {
            if (!state.swr) {
                // nothing to do
                free(data.data);
                break;
            }
            int ret;
            // number of samples = bytes / channels / (bps / 8)
            const int srcsamples = data.size / state.srcchannels / (bitsPerSample(srcfmt.format) / 8);
            const int dstsamples = av_rescale_rnd(swr_get_delay(state.swr, srcfmt.rate) + srcsamples,
                                                  dstfmt.rate, srcfmt.rate, AV_ROUND_UP);
            if (dstsamples > state.maxdstsamples) {
                av_free(state.dstdata[0]);
                ret = av_samples_alloc(state.dstdata, &state.dstline, state.dstchannels,
                                       dstsamples, dstfmt.format, 1);
                if (ret < 0) {
                    r->throwError("Unable to resize destination sample data");
                    free(data.data);
                    reset();
                    break;
                }
                state.maxdstsamples = dstsamples;
                lastsrcsamples = srcsamples;
            }

            ret = swr_convert(state.swr, state.dstdata, dstsamples, (const uint8_t**)&data.data, srcsamples);
            if (ret < 0) {
                r->throwError("Unable to convert samples");
                free(data.data);
                reset();
                break;
            }
            if (!ret) {
                // ???
                free(data.data);
                break;
            }

            const size_t dstsize = av_samples_get_buffer_size(&state.dstline, state.dstchannels, ret, dstfmt.format, 1);
            // copy data to a new Data and send to JS
            uint8_t* ptr = reinterpret_cast<uint8_t*>(malloc(dstsize));
            memcpy(ptr, state.dstdata[0], dstsize);
            r->output.push({ Data::Samples, ptr, dstsize, {} });
            uv_async_send(&r->async);

            free(data.data);

            break; }
        case Data::End:
            r->output.push({ Data::End, nullptr, 0, {} });
            uv_async_send(&r->async);
            break;
        }
    }
}

v8::Local<v8::Object> Resample::makeObject()
{
    Nan::EscapableHandleScope scope;
    v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>();
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::Function> ctor = Nan::GetFunction(tpl).ToLocalChecked();
    v8::Local<v8::Object> obj = Nan::NewInstance(ctor, 0, nullptr).ToLocalChecked();
    Wrap(obj);
    return scope.Escape(obj);
}

NAN_METHOD(create) {
    Resample* resample = new Resample;
    resample->open();
    info.GetReturnValue().Set(resample->makeObject());
}

static Resample::Format makeFormat(v8::Local<v8::Object> obj)
{
    Nan::HandleScope scope;
    auto chKey = Nan::New<v8::String>("channels").ToLocalChecked();
    auto rateKey = Nan::New<v8::String>("rate").ToLocalChecked();
    auto fmtKey = Nan::New<v8::String>("format").ToLocalChecked();
    if (!obj->Has(chKey) || !obj->Has(rateKey) || !obj->Has(fmtKey)) {
        return Resample::Format{};
    }
    auto chVal = obj->Get(chKey);
    auto rateVal = obj->Get(rateKey);
    auto fmtVal = obj->Get(fmtKey);
    if (!chVal->IsUint32() || !rateVal->IsUint32() || !fmtVal->IsString()) {
        return Resample::Format{};
    }
    const std::string fmtstr = *Nan::Utf8String(fmtVal);
    Resample::Format fmt{};
    if (fmtstr == "u8") {
        fmt.format = AV_SAMPLE_FMT_U8;
    } else if (fmtstr == "s16") {
        fmt.format = AV_SAMPLE_FMT_S16;
    } else if (fmtstr == "s32") {
        fmt.format = AV_SAMPLE_FMT_S32;
    } else if (fmtstr == "flt") {
        fmt.format = AV_SAMPLE_FMT_FLT;
    } else if (fmtstr == "dbl") {
        fmt.format = AV_SAMPLE_FMT_DBL;
    } else {
        return fmt;
    }
    switch (v8::Local<v8::Uint32>::Cast(chVal)->Value()) {
    case 1:
        fmt.channels = AV_CH_LAYOUT_MONO;
        break;
    case 2:
        fmt.channels = AV_CH_LAYOUT_STEREO;
        break;
    default:
        return fmt;
    }
    fmt.rate = v8::Local<v8::Uint32>::Cast(rateVal)->Value();
    return fmt;
}

NAN_METHOD(setSourceFormat) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for format");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsObject()) {
        Nan::ThrowError("Need an object for format");
        return;
    }
    Resample* resample = Resample::Unwrap<Resample>(v8::Local<v8::Object>::Cast(info[0]));
    Resample::Format fmt = makeFormat(v8::Local<v8::Object>::Cast(info[1]));
    if (!fmt.rate) {
        Nan::ThrowError("Unable to make format");
        return;
    }
    resample->input.push({ Resample::Data::SrcFmt, nullptr, 0, std::move(fmt) });
}

NAN_METHOD(setDestinationFormat) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for format");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsObject()) {
        Nan::ThrowError("Need an object for format");
        return;
    }
    Resample* resample = Resample::Unwrap<Resample>(v8::Local<v8::Object>::Cast(info[0]));
    Resample::Format fmt = makeFormat(v8::Local<v8::Object>::Cast(info[1]));
    if (!fmt.rate) {
        Nan::ThrowError("Unable to make format");
        return;
    }
    resample->input.push({ Resample::Data::DstFmt, nullptr, 0, std::move(fmt) });
}

NAN_METHOD(feed) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for format");
        return;
    }
    if (info.Length() < 2 || !node::Buffer::HasInstance(info[1])) {
        Nan::ThrowError("Need a buffer for format");
        return;
    }
    size_t length = 0;
    if (info.Length() >= 3 && info[2]->IsUint32()) {
        // got a length
        length = v8::Local<v8::Uint32>::Cast(info[2])->Value();
    }
    const char* data = node::Buffer::Data(info[1]);
    if (!length)
        length = node::Buffer::Length(info[1]);
    if (!length)
        return;

    // copy? could keep a persistent to the buffer and just pass the pointer
    // but then the caller would have to promise not to change the buffer in JS
    uint8_t* ptr = reinterpret_cast<uint8_t*>(malloc(length));
    memcpy(ptr, data, length);

    Resample* resample = Resample::Unwrap<Resample>(v8::Local<v8::Object>::Cast(info[0]));
    resample->input.push({ Resample::Data::Samples, ptr, length, {} });
}

NAN_METHOD(end) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for format");
        return;
    }
    Resample* resample = Resample::Unwrap<Resample>(v8::Local<v8::Object>::Cast(info[0]));
    resample->input.push({ Resample::Data::End, nullptr, 0, {} });
}

NAN_METHOD(on) {
    if (info.Length() < 1 || !info[0]->IsObject()) {
        Nan::ThrowError("Need an external for on");
        return;
    }
    if (info.Length() < 2 || !info[1]->IsString()) {
        Nan::ThrowError("Need a string for on");
        return;
    }
    if (info.Length() < 3 || !info[2]->IsFunction()) {
        Nan::ThrowError("Need a function for on");
        return;
    }
    Resample* resample = Resample::Unwrap<Resample>(v8::Local<v8::Object>::Cast(info[0]));
    const std::string name = *Nan::Utf8String(info[1]);
    resample->ons[name].push_back(std::make_shared<Nan::Callback>(v8::Local<v8::Function>::Cast(info[2])));
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, create);
    NAN_EXPORT(target, setSourceFormat);
    NAN_EXPORT(target, setDestinationFormat);
    NAN_EXPORT(target, feed);
    NAN_EXPORT(target, end);
    NAN_EXPORT(target, on);
}

NODE_MODULE(swresample, Initialize)
