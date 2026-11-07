// SPDX-License-Identifier: GPL-3.0-or-later
// Run with Node.js to regenerate the NovaS SDL and Windows icons.
const fs = require('node:fs');
const path = require('node:path');

const background = [0x16, 0x17, 0x1c];
const foreground = [0x2e, 0xc5, 0xf0];
const stroke = [[69, 32], [42, 32], [32, 42], [42, 50], [58, 50], [68, 58], [58, 68], [31, 68]];

function distance(x, y, a, b) {
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const t = Math.max(0, Math.min(1, ((x - a[0]) * dx + (y - a[1]) * dy) / (dx * dx + dy * dy)));
    return Math.hypot(x - a[0] - t * dx, y - a[1] - t * dy);
}

function pixels(size) {
    const data = Buffer.alloc(size * size * 4);
    for (let y = 0; y < size; ++y) {
        for (let x = 0; x < size; ++x) {
            let hits = 0;
            for (let sy = 0; sy < 4; ++sy) {
                for (let sx = 0; sx < 4; ++sx) {
                    const px = (x + (sx + 0.5) / 4) * 100 / size;
                    const py = (y + (sy + 0.5) / 4) * 100 / size;
                    const ring = Math.abs(Math.hypot(px - 50, py - 50) - 42) < 2.5;
                    const s = stroke.slice(1).some((b, i) => distance(px, py, stroke[i], b) < 4.5);
                    if (ring || s) ++hits;
                }
            }
            const offset = ((size - 1 - y) * size + x) * 4;
            for (let c = 0; c < 3; ++c) {
                data[offset + 2 - c] = Math.round(background[c] + (foreground[c] - background[c]) * hits / 16);
            }
            data[offset + 3] = 255;
        }
    }
    return data;
}

function dib(size, icon) {
    const data = pixels(size);
    const mask = icon ? Buffer.alloc(Math.ceil(size / 32) * 4 * size) : Buffer.alloc(0);
    const header = Buffer.alloc(40);
    header.writeUInt32LE(40, 0);
    header.writeInt32LE(size, 4);
    header.writeInt32LE(icon ? size * 2 : size, 8);
    header.writeUInt16LE(1, 12);
    header.writeUInt16LE(32, 14);
    header.writeUInt32LE(data.length + mask.length, 20);
    return Buffer.concat([header, data, mask]);
}

const bitmap = dib(128, false);
const bmpHeader = Buffer.alloc(14);
bmpHeader.write('BM');
bmpHeader.writeUInt32LE(14 + bitmap.length, 2);
bmpHeader.writeUInt32LE(54, 10);
fs.writeFileSync(path.join(__dirname, 'novas.bmp'), Buffer.concat([bmpHeader, bitmap]));

const sizes = [16, 24, 32, 48, 64, 128, 256];
const images = sizes.map(size => dib(size, true));
const directory = Buffer.alloc(6 + sizes.length * 16);
directory.writeUInt16LE(1, 2);
directory.writeUInt16LE(sizes.length, 4);
let offset = directory.length;
sizes.forEach((size, i) => {
    const entry = 6 + i * 16;
    directory[entry] = size === 256 ? 0 : size;
    directory[entry + 1] = directory[entry];
    directory.writeUInt16LE(1, entry + 4);
    directory.writeUInt16LE(32, entry + 6);
    directory.writeUInt32LE(images[i].length, entry + 8);
    directory.writeUInt32LE(offset, entry + 12);
    offset += images[i].length;
});
fs.writeFileSync(path.join(__dirname, 'novas.ico'), Buffer.concat([directory, ...images]));
