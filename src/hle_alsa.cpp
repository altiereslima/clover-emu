// HLE of the libasound PCM API (used by ReedPlayer for UI sound) on top of SDL queued audio.
#include "hle.h"
#include <SDL.h>

struct AlsaPcm {
    SDL_AudioDeviceID dev = 0;
    u32 rate = 48000, channels = 2, format = 2;  // SND_PCM_FORMAT_S16_LE
    u32 buffer_frames = 4096;
    bool prepared = false;
};

static SDL_AudioFormat to_sdl_fmt(u32 f) {
    switch (f) {
    case 0: return AUDIO_S8;
    case 1: return AUDIO_U8;
    case 2: return AUDIO_S16LSB;
    case 3: return AUDIO_S16MSB;
    case 10: return AUDIO_S32LSB;
    case 14: return AUDIO_F32LSB;
    }
    return AUDIO_S16LSB;
}
static u32 frame_bytes(const AlsaPcm& p) {
    u32 s = (p.format == 10 || p.format == 14) ? 4 : (p.format <= 1 ? 1 : 2);
    return s * p.channels;
}

static std::shared_ptr<AlsaPcm> get_pcm(HleArgs& a, u32 h) {
    auto& st = hle_state(*a.t.proc);
    std::lock_guard lk(st.mtx);
    auto it = st.pcms.find(h);
    return it == st.pcms.end() ? nullptr : it->second;
}

// hw_params layout (guest-owned blob): [0]=format [4]=channels [8]=rate [12]=buffer_frames [16]=access
HLE(LIB_ASOUND, snd_pcm_open) {
    auto& st = hle_state(*a.t.proc);
    u32 h = st.alloc(32);
    auto pcm = std::make_shared<AlsaPcm>();
    {
        std::lock_guard lk(st.mtx);
        st.pcms[h] = pcm;
    }
    a.m.wr<u32>(a.u(0), h);
    LOGC(LOG_HLE, "[alsa] pid %d open '%s' stream %d -> %08x\n", a.t.proc->pid, a.str(1).c_str(), a.s(2), h);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_hw_params_sizeof) { a.ret(64); }
HLE(LIB_ASOUND, snd_pcm_hw_params_any) {
    u32 p = a.u(1);
    memset(a.m.ptr(p), 0, 64);
    a.m.wr<u32>(p + 0, 2);
    a.m.wr<u32>(p + 4, 2);
    a.m.wr<u32>(p + 8, 48000);
    a.m.wr<u32>(p + 12, 4096);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_hw_params_set_access) { a.m.wr<u32>(a.u(1) + 16, a.u(2)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_set_format) { a.m.wr<u32>(a.u(1) + 0, a.u(2)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_set_channels) { a.m.wr<u32>(a.u(1) + 4, a.u(2)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_set_rate_near) {
    u32 r = a.m.rd<u32>(a.u(2));
    a.m.wr<u32>(a.u(1) + 8, r);
    if (a.u(3)) a.m.wr<s32>(a.u(3), 0);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_hw_params_set_rate) { a.m.wr<u32>(a.u(1) + 8, a.u(2)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_set_buffer_size) { a.m.wr<u32>(a.u(1) + 12, a.u(2)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_set_buffer_size_near) { a.m.wr<u32>(a.u(1) + 12, a.m.rd<u32>(a.u(2))); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_set_period_size_near) { a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_get_channels) { a.m.wr<u32>(a.u(1), a.m.rd<u32>(a.u(0) + 4)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params_get_rate) {
    a.m.wr<u32>(a.u(1), a.m.rd<u32>(a.u(0) + 8));
    if (a.u(2)) a.m.wr<s32>(a.u(2), 0);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_hw_params_get_buffer_size) { a.m.wr<u32>(a.u(1), a.m.rd<u32>(a.u(0) + 12)); a.ret(0); }
HLE(LIB_ASOUND, snd_pcm_hw_params) {
    auto pcm = get_pcm(a, a.u(0));
    if (!pcm) { a.ret((u32)-77); return; }
    u32 p = a.u(1);
    pcm->format = a.m.rd<u32>(p + 0);
    pcm->channels = a.m.rd<u32>(p + 4);
    pcm->rate = a.m.rd<u32>(p + 8);
    pcm->buffer_frames = a.m.rd<u32>(p + 12);
    if (pcm->dev) SDL_CloseAudioDevice(pcm->dev);
    SDL_AudioSpec want{}, have{};
    want.freq = (int)pcm->rate;
    want.format = to_sdl_fmt(pcm->format);
    want.channels = (u8)pcm->channels;
    want.samples = 1024;
    pcm->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!pcm->dev) LOGI("[alsa] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(pcm->dev, 0);
    LOGC(LOG_HLE, "[alsa] hw_params rate %u ch %u fmt %u buffer %u\n", pcm->rate, pcm->channels, pcm->format, pcm->buffer_frames);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_prepare) {
    auto pcm = get_pcm(a, a.u(0));
    if (pcm) pcm->prepared = true;
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_writei) {
    auto pcm = get_pcm(a, a.u(0));
    u32 frames = a.u(2);
    if (!pcm || !pcm->dev) { a.ret(frames); return; }
    u32 fb = frame_bytes(*pcm);
    // block while too much is queued (keeps the guest paced like a real PCM device)
    u32 limit = std::max<u32>(pcm->buffer_frames, 2048) * fb;
    while (SDL_GetQueuedAudioSize(pcm->dev) > limit) {
        if (sleep_ns(a.t, 2000000ull) != 0) { a.ret((u32)-4); return; }
    }
    SDL_QueueAudio(pcm->dev, a.m.ptr(a.u(1)), frames * fb);
    a.ret(frames);
}
HLE(LIB_ASOUND, snd_pcm_drain) {
    auto pcm = get_pcm(a, a.u(0));
    if (pcm && pcm->dev) {
        for (int i = 0; i < 200 && SDL_GetQueuedAudioSize(pcm->dev) > 0; i++) sleep_ns(a.t, 5000000ull);
    }
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_drop) {
    auto pcm = get_pcm(a, a.u(0));
    if (pcm && pcm->dev) SDL_ClearQueuedAudio(pcm->dev);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_pcm_close) {
    auto& st = hle_state(*a.t.proc);
    std::shared_ptr<AlsaPcm> pcm;
    {
        std::lock_guard lk(st.mtx);
        auto it = st.pcms.find(a.u(0));
        if (it != st.pcms.end()) {
            pcm = it->second;
            st.pcms.erase(it);
        }
    }
    if (pcm && pcm->dev) SDL_CloseAudioDevice(pcm->dev);
    a.ret(0);
}
HLE(LIB_ASOUND, snd_strerror) { a.ret(hle_state(*a.t.proc).intern("alsa error")); }

void alsa_process_cleanup(HleProcState& st) {
    std::map<u32, std::shared_ptr<AlsaPcm>> pcms;
    {
        std::lock_guard lk(st.mtx);
        pcms.swap(st.pcms);
    }
    for (auto& [h, p] : pcms)
        if (p->dev) SDL_CloseAudioDevice(p->dev);
}
