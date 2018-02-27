/*global require*/
const { ResampleStream } = require("..");

const fs = require("fs");
const rs = fs.createReadStream("testin.raw");
const tr = new ResampleStream({
    sourceFormat: { format: "s16", rate: 44100, channels: 2 },
    destinationFormat: { format: "s16", rate: 8000, channels: 1 }
});
const ws = fs.createWriteStream("testout2.raw");
rs.pipe(tr).pipe(ws);
