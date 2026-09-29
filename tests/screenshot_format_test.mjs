import assert from 'node:assert/strict';
import { inflateSync } from 'node:zlib';
import { test } from 'node:test';
import { crc32, rgb565ToPng, validateFrame } from '../mac/screenshot-format.mjs';

test('CRC32 uses the standard serial-frame checksum', () => {
  assert.equal(crc32(Buffer.from('123456789')), 0xcbf43926);
});

test('RGB565 pixels become a valid RGB PNG', () => {
  const raw = Buffer.from([0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00]);
  const png = rgb565ToPng(raw, 3, 1);
  assert.deepEqual(png.subarray(0, 8), Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  assert.equal(png.readUInt32BE(16), 3);
  assert.equal(png.readUInt32BE(20), 1);
  const idat = png.indexOf('IDAT');
  const inflated = inflateSync(png.subarray(idat + 4, idat + 4 + png.readUInt32BE(idat - 4)));
  assert.deepEqual(inflated, Buffer.from([0, 255, 0, 0, 0, 255, 0, 0, 0, 255]));
});

test('a corrupt or incomplete frame is rejected', () => {
  const raw = Buffer.from([0x00, 0xf8]);
  assert.throws(() => validateFrame(raw, 1, 1, 0), /checksum/);
  assert.throws(() => validateFrame(raw, 2, 1, crc32(raw)), /length/);
});
