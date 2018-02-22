/*global require,module,process*/

const { Transform } = require("stream");
const SwResample = require("bindings")("swresample.node");

class Resample extends Transform {
    constructor(options) {
        super(options);

        if (!options.destinationFormat) {
            throw new Error("Needs a destination format");
        }

        this._resample = SwResample.create();
        SwResample.open(this._resample);

        this._index = 0;
        this._callbacks = new Map();
        this._opened = true;

        if (options.sourceFormat) {
            this._sourceFormat = options.sourceFormat;
            SwResample.setSourceFormat(this._resample, options.sourceFormat);
        }
        SwResample.setDestinationFormat(this._resample, options.destinationFormat);
        SwResample.on(this._resample, "samples", (buf, index) => {
            let cb = this._callbacks[index];
            delete this._callbacks[index];
            if (cb) {
                cb(null);
            } else {
                throw new Error("No callback for", index);
            }
            //console.log("transformed", buf);
            this.push(buf);
        });
        SwResample.on(this._resample, "end", () => {
            if (this._flushCallback) {
                this._flushCallback();
                this._flushCallback = undefined;
            }
            this._opened = false;
            SwResample.close(this._resample);
        });
        SwResample.on(this._resample, "error", err => {
            console.error(err.message);
        });

        this.on("pipe", src => {
            if (!this._opened) {
                SwResample.open(this._resample);
                this._opened = true;
            }
            src.on("format", this._sourceFormatChanged);
        });
        this.on("unpipe", src => {
            src.removeListener("format", this._sourceFormatChanged);
        });
    }

    _flush(callback) {
        this._flushCallback = callback;
        SwResample.end(this._resample);
    }

    _transform(chunk, encoding, callback) {
        //console.log(chunk);
        this._callbacks[this._index] = callback;
        SwResample.feed(this._resample, this._index++, chunk);
    }

    _isSameFormat(fmt1, fmt2) {
        if (!fmt1 && !fmt2)
            return true;
        if (!fmt1 || !fmt2)
            return false;
        if (fmt1.channels != fmt2.channels
            || fmt1.rate != fmt2.rate
            || fmt1.format != fmt2.format)
            return false;
        return true;
    }

    _sourceFormatChanged(srcfmt) {
        if (!this._isSameFormat(srcfmt, this._sourceFormat)) {
            this._sourceFormat = srcfmt;
            SwResample.setSourceFormat(this._resample, srcfmt);
        }
    }
}

module.exports = Resample;
