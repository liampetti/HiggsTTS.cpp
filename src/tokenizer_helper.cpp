// tokenizer_helper.cpp — Load model + prefill → build prompt → dump prompt_ids to txt.
// Usage: tokenizer_helper --model <gguf> --ref-wav <wav> [--text <str>]
//                         [--ref-text <str>] [--tokenizer <tokenizer.json>] [--out <file>]
//
// Three comparison runs:
//   1. Python side (reference)
//   2. C++ bpe:   tokenizer_helper --model m.gguf --ref-wav r.wav --text "..." --ref-text "..." --out bpe.txt
//   3. C++ hf:    tokenizer_helper --model m.gguf --ref-wav r.wav --text "..." --ref-text "..." --tokenizer tokenizer.json --out hf.txt

#include "higgs_tts.h"
#include "higgs_prefill.h"
#include "higgs_BAR.h"
#include "core/bpe.h"
#include "core/hf_tokenizer.h"
#include "core/audio_resample.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ── Windows Unicode argv ─────────────────────────────────────────────────────
#ifdef _WIN32
static std::string utf16_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], len, nullptr, nullptr);
    return out;
}
static std::vector<std::string> get_utf8_argv() {
    std::vector<std::string> args;
    int argc;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return args;
    for (int i = 0; i < argc; i++)
        args.push_back(utf16_to_utf8(wargv[i]));
    LocalFree(wargv);
    return args;
}
#endif

// ── WAV reader ────────────────────────────────────────────────────────────────
static bool read_wav_mono_24k(const char* path, std::vector<float>& samples) {
    unsigned int channels = 0, sr = 0;
    drwav_uint64 total = 0;
    float* data = drwav_open_file_and_read_pcm_frames_f32(path, &channels, &sr, &total, nullptr);
    if (!data) { fprintf(stderr, "wav read fail: %s\n", path); return false; }
    std::vector<float> mono(total);
    for (drwav_uint64 i = 0; i < total; i++) {
        mono[i] = 0.0f;
        for (unsigned int c = 0; c < channels; c++)
            mono[i] += data[i * channels + c];
        mono[i] /= (float)channels;
    }
    drwav_free(data, nullptr);
    if ((int)sr != 24000)
        samples = core_audio::resample_polyphase(mono.data(), (int)mono.size(), (int)sr, 24000);
    else
        samples = std::move(mono);
    return true;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    auto utf8_args = get_utf8_argv();
    int nargs = (int)utf8_args.size();
    std::vector<const char*> cargs(nargs);
    for (int i = 0; i < nargs; i++) cargs[i] = utf8_args[i].c_str();
    argv = (char**)cargs.data();
    argc = nargs;
#endif

    const char* model_path = nullptr;
    const char* ref_wav    = nullptr;
    const char* text       = "I want a hero: an uncommon want";
    const char* ref_text   = nullptr;
    const char* tok_path   = nullptr;
    const char* out_path   = "prompt_ids.txt";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model_path = argv[++i];
        else if (!strcmp(argv[i], "--tokenizer") && i + 1 < argc)
            tok_path = argv[++i];
        else if (!strcmp(argv[i], "--ref-wav") && i + 1 < argc)
            ref_wav = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc)
            text = argv[++i];
        else if (!strcmp(argv[i], "--ref-text") && i + 1 < argc)
            ref_text = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
    }
    if (!model_path || !ref_wav) {
        fprintf(stderr, "Usage: tokenizer_helper --model <gguf> --ref-wav <wav> [--text <str>] [--ref-text <str>] [--tokenizer <tokenizer.json>] [--out <file>]\n");
        return 1;
    }

    // Load model
    higgs_test_model m;
    if (!higgs_test_load_vocab(model_path, &m)) { fprintf(stderr, "vocab fail\n"); return 1; }
    if (!higgs_test_load(model_path, &m))       { fprintf(stderr, "load fail\n"); return 1; }

    // Prefill encode
    std::vector<float> wav_samples;
    if (!read_wav_mono_24k(ref_wav, wav_samples)) { higgs_test_free(&m); return 1; }
    std::vector<int32_t> codes;
    int T_frames = 0;
    if (!higgs_prefill_encode(&m, wav_samples.data(), (int)wav_samples.size(), codes, T_frames)) {
        fprintf(stderr, "Prefill encode failed\n");
        higgs_test_free(&m); return 1;
    }

    // Tokenize
    HFTokenizer hf_tok;
    const char* mode_label = "bpe (GGUF vocab)";
    if (tok_path && hf_tok.load(tok_path))
        mode_label = "hf (tokenizer.json)";

    auto tokenize = [&](const char* t) -> std::vector<int32_t> {
        if (tok_path) return hf_tok.encode(t);
        return core_bpe::tokenize_simple(m.token_to_id, m.merge_rank, t);
    };
    std::vector<int32_t> target_tokens = tokenize(text);
    std::vector<int32_t> ref_text_tokens;
    if (ref_text) ref_text_tokens = tokenize(ref_text);

    // Build prompt
    int N = 8;
    int L_audio = T_frames + N - 1;
    auto prompt_ids = higgs_build_prompt(&m, target_tokens, ref_text_tokens, L_audio, true);

    // Stats
    int neg100_count = 0;
    for (int pid : prompt_ids) if (pid == -100) neg100_count++;

    printf("=== tokenizer_helper [%s] ===\n", mode_label);
    printf("Model:     %s\n", model_path);
    printf("Ref WAV:   %s  (%zu samples, %d frames)\n", ref_wav, wav_samples.size(), T_frames);
    printf("Text:      \"%s\"\n", text);
    if (ref_text) printf("Ref text:  \"%s\"\n", ref_text);
    printf("prompt_ids: %zu tokens, -100 count: %d\n", prompt_ids.size(), neg100_count);

    // Dump to file
    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "Cannot open output: %s\n", out_path);
        higgs_test_free(&m); return 1;
    }
    fprintf(f, "# tokenizer_helper [%s]\n", mode_label);
    fprintf(f, "# model: %s\n", model_path);
    fprintf(f, "# ref-wav: %s  (%zu samples, %d frames)\n", ref_wav, wav_samples.size(), T_frames);
    fprintf(f, "# text: %s\n", text);
    if (ref_text) fprintf(f, "# ref-text: %s\n", ref_text);
    fprintf(f, "# total: %zu tokens, -100: %d\n", prompt_ids.size(), neg100_count);
    fprintf(f, "#\n");
    fprintf(f, "# prompt_ids (one per line):\n");
    for (int id : prompt_ids)
        fprintf(f, "%d\n", id);

    fclose(f);
    printf("Dumped: %s\n", out_path);
    higgs_test_free(&m);
    return 0;
}
