import { deflateSync } from 'node:zlib';

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function validateFrame(raw, width, height, checksum) {
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) ||
      width < 1 || height < 1 || width > 4096 || height > 4096 ||
      raw.length !== width * height * 2) throw new Error('Invalid RGB565 frame length');
  if (crc32(raw) !== (checksum >>> 0)) throw new Error('Screenshot checksum mismatch');
}

function pngChunk(name, data) {
  const type = Buffer.from(name, 'ascii');
  const chunk = Buffer.alloc(12 + data.length);
  chunk.writeUInt32BE(data.length, 0);
  type.copy(chunk, 4);
  data.copy(chunk, 8);
  chunk.writeUInt32BE(crc32(chunk.subarray(4, 8 + data.length)), 8 + data.length);
  return chunk;
}

export function rgb565ToPng(raw, width, height) {
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) ||
      width < 1 || height < 1 || width > 4096 || height > 4096 ||
      raw.length !== width * height * 2) throw new Error('Invalid RGB565 frame length');
  const pixels = Buffer.alloc(height * (1 + width * 3));
  for (let y = 0; y < height; y++) {
    const row = y * (1 + width * 3);
    for (let x = 0; x < width; x++) {
      const color = raw.readUInt16LE((y * width + x) * 2);
      const offset = row + 1 + x * 3;
      pixels[offset] = ((color >> 11) & 31) * 255 / 31;
      pixels[offset + 1] = ((color >> 5) & 63) * 255 / 63;
      pixels[offset + 2] = (color & 31) * 255 / 31;
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bits per channel
  ihdr[9] = 2; // RGB
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', deflateSync(pixels)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}
