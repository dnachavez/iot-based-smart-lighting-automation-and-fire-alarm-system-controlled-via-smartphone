// Runtime-synthesized siren: 1.6 s of alternating 600 Hz / 1100 Hz tones,
// 8 kHz / 16-bit mono PCM, wrapped in a WAV header and base64-encoded.
// Loaded into expo-av via a data URI so the repo doesn't need to ship a
// binary audio asset.

const SAMPLE_RATE = 8000;
const SECONDS = 1.6;
const PHASE_SECONDS = 0.4;
const FREQ_LOW = 600;
const FREQ_HIGH = 1100;
const AMPLITUDE = 0.7;

const B64_ALPHABET =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

const uint8ToBase64 = (bytes: Uint8Array): string => {
  let result = '';
  let i = 0;
  const len = bytes.length;
  for (; i + 2 < len; i += 3) {
    const a = bytes[i];
    const b = bytes[i + 1];
    const c = bytes[i + 2];
    result +=
      B64_ALPHABET[a >> 2] +
      B64_ALPHABET[((a & 0x03) << 4) | (b >> 4)] +
      B64_ALPHABET[((b & 0x0f) << 2) | (c >> 6)] +
      B64_ALPHABET[c & 0x3f];
  }
  const remaining = len - i;
  if (remaining === 1) {
    const a = bytes[i];
    result += B64_ALPHABET[a >> 2] + B64_ALPHABET[(a & 0x03) << 4] + '==';
  } else if (remaining === 2) {
    const a = bytes[i];
    const b = bytes[i + 1];
    result +=
      B64_ALPHABET[a >> 2] +
      B64_ALPHABET[((a & 0x03) << 4) | (b >> 4)] +
      B64_ALPHABET[(b & 0x0f) << 2] +
      '=';
  }
  return result;
};

const buildSirenWav = (): Uint8Array => {
  const totalSamples = Math.floor(SAMPLE_RATE * SECONDS);
  const byteLen = totalSamples * 2;
  const buf = new Uint8Array(44 + byteLen);
  const view = new DataView(buf.buffer);

  // RIFF header
  buf[0] = 0x52; buf[1] = 0x49; buf[2] = 0x46; buf[3] = 0x46; // "RIFF"
  view.setUint32(4, 36 + byteLen, true);
  buf[8] = 0x57; buf[9] = 0x41; buf[10] = 0x56; buf[11] = 0x45; // "WAVE"

  // fmt subchunk (PCM)
  buf[12] = 0x66; buf[13] = 0x6d; buf[14] = 0x74; buf[15] = 0x20; // "fmt "
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true); // PCM
  view.setUint16(22, 1, true); // mono
  view.setUint32(24, SAMPLE_RATE, true);
  view.setUint32(28, SAMPLE_RATE * 2, true); // byte rate
  view.setUint16(32, 2, true); // block align
  view.setUint16(34, 16, true); // bits per sample

  // data subchunk
  buf[36] = 0x64; buf[37] = 0x61; buf[38] = 0x74; buf[39] = 0x61; // "data"
  view.setUint32(40, byteLen, true);

  // Synthesize: alternating frequency blocks with a short attack/release envelope.
  for (let i = 0; i < totalSamples; i++) {
    const t = i / SAMPLE_RATE;
    const phaseIndex = Math.floor(t / PHASE_SECONDS) % 2;
    const freq = phaseIndex === 0 ? FREQ_LOW : FREQ_HIGH;
    const tone = Math.sin(2 * Math.PI * freq * t);
    // 20 ms attack/release so the clip can loop without click artifacts.
    const attack = Math.min(1, t * 50);
    const release = Math.min(1, (SECONDS - t) * 50);
    const env = attack * release;
    const sample = Math.round(tone * env * AMPLITUDE * 32767);
    view.setInt16(44 + i * 2, sample, true);
  }
  return buf;
};

// Synthesize once per process. ~25 kB raw, ~34 kB base64-encoded.
const SIREN_WAV_BASE64 = uint8ToBase64(buildSirenWav());

export const ALARM_WAV_DATA_URI = `data:audio/wav;base64,${SIREN_WAV_BASE64}`;
