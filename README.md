# node-swresample
Node module for resampling audio using ffmpegs libswresample

## Installation
```npm install --save swresample```

## Dependencies
This module needs libswresample installed.

#### Debian-like Linux
```sudo apt install libswresample-dev```

#### OS X
```brew install ffmpeg```

## Example
```javascript
const { ResampleStream } = require("swresample");
const fs = require("fs");

const readStream = fs.createReadStream("in.raw");
const resampleStream = new ResampleStream({
    sourceFormat: { format: "s16", rate: 44100, channels: 2 },
    destinationFormat: { format: "s16", rate: 8000, channels: 1 }
});
const writeStream = fs.createWriteStream("out.raw");

readStream.pipe(resampleStream).pipe(writeStream);
```
