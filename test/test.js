/*global require*/
const Resample = require("..");

const fs = require("fs");
const rs = fs.createReadStream("testin.wav");
const tr = new Resample({
    sourceFormat: { format: "s16", rate: 44100, channels: 2 },
    destinationFormat: { format: "s16", rate: 8000, channels: 1 }
});
const ws = fs.createWriteStream("testout2.wav");
rs.pipe(tr).pipe(ws);
