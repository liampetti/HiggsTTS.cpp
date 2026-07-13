# HiggsTTS.cpp

ggml port of [bosonai/higgs-tts-3-4b](https://huggingface.co/bosonai/higgs-tts-3-4b).

See [LICENSE-HIGGS](LICENSE-HIGGS).

## Build

Personally I recommend Vulkan — it's fast and supports all platforms.
CUDA and HIP work too.

```bash
# Vulkan (recommended)
cmake -B build-vk -DGGML_VULKAN=ON
cmake --build build-vk --config Release -j

# CUDA
cmake -B build-cu -DGGML_CUDA=ON
cmake --build build-cu --config Release -j

# HIP (AMD)
cmake -B build-hip -DGGML_HIP=ON -DHIP_PLATFORM=amd
cmake --build build-hip --config Release -j
```

## Model

Download GGUF from [NeemaShioSe/HiggsTTS3.gguf](https://huggingface.co/NeemaShioSe/HiggsTTS3.gguf).

or you can convert the model yourself, see [here](https://github.com/Rafa00127/HiggsTTS.cpp/tree/main/convert_model)

## Usage

```bash
# CLI: WAV → WAV
higgs_cli --model higgs-v3-tts.gguf --ref-wav ref.wav --text "Hello world" --out out.wav

# CLI with emotion tags (optional, requires tokenizer.json)
higgs_cli --model higgs-v3-tts.gguf --ref-wav ref.wav --text "Hello world" --tokenizer tokenizer.json --out out.wav

# Simple TCP server: accepts text + temperature → streams float32 PCM
higgs_server --model higgs-v3-tts.gguf --ref-wav ref.wav --ref-text "reference transcript" --port 9989

# Server with emotion tags (optional, requires tokenizer.json)
higgs_server --model higgs-v3-tts.gguf --ref-wav ref.wav --ref-text "reference transcript" --tokenizer tokenizer.json --port 9989
```

### TCP protocol

The client sends a 4-byte big-endian UTF-8 byte length, a 4-byte big-endian
IEEE-754 temperature, then the UTF-8 text. The server responds with frames:

| Type | Payload |
|------|---------|
| `1` | Float32 little-endian PCM at 24 kHz |
| `2` | Empty end-of-stream marker |
| `3` | UTF-8 error text |

Each response frame is `type:u8 + payload_bytes:u32be + payload`. PCM arrives
incrementally as stable codec windows are decoded. The server keeps a short
final tail until generation ends, then trims trailing silence before sending it.
`--max-actions` caps autoregressive decode and KV-cache allocation; use `0`
(the default) for the model-derived limit, or a value of at least `8`.

`--tokenizer` is optional. It enables recognition of special emotion / style / prosody tags
(e.g. `<|style:whispering|>`)
when they appear in the prompt text. Without it, the model falls back to the built-in
GGUF BPE tokenizer.

## GUI

### PyQt GUI (Python + TCP server)

A simple PyQt6 GUI that connects to `higgs_server` over TCP:
[python_gui/simple_tts_gui.py](python_gui/simple_tts_gui.py).

```bash
pip install pyqt6 numpy sounddevice
# Start the server first, then launch the GUI
python python_gui/simple_tts_gui.py
```

Configure model paths in Model Config, launch the server from the GUI,
then type text and click Synthesize.

### Windows GUI (WPF + C# bindings)

A native WPF GUI with direct C ABI bindings is provided in [CSharpBinding/](CSharpBinding/).
Runs local inference — no server needed.

```bash
# Build the native DLL
cmake --build build --target higgs_tts --config Release

# Build the .NET GUI
dotnet build CSharpBinding/HiggsTTS.net/HiggsTTSGUI/HiggsTTSGUI.csproj -c Release
```

Copy `higgs_tts.dll` and its dependencies (e.g. `ggml.dll`, `ggml-cpu.dll`, `ggml-vulkan.dll`)
from `build/bin/` to the GUI output directory before running.

The Tag dropdown gives quick access to all emotion / style / prosody tokens.
Built-in audio player supports play / pause / seek.


## Special Token Reference

See [PROMPTING.md](https://huggingface.co/bosonai/higgs-tts-3-4b/blob/main/PROMPTING.md) for full details.

| Category | Examples |
|----------|----------|
| Emotion (21) | `emotion:elation` `emotion:anger` `emotion:sadness` `emotion:fear` ... |
| Style (3) | `style:singing` `style:shouting` `style:whispering` |
| SFX (9) | `sfx:laughter` `sfx:cough` `sfx:sigh` `sfx:sneeze` ... |
| Prosody (10) | `prosody:speed_slow` `prosody:pitch_high` `prosody:pause` ... |

Tags are used as `<|category:name|>` in the prompt, e.g. `<|emotion:elation|> Hello world!`

Prepend a tag to your text to control delivery: `<|emotion:elation|> Hello world!`
