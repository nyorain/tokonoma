#include <tkn/sampling.hpp>
#include <tkn/audio.hpp>
#include <tkn/sound.hpp>
#include <dlg/dlg.hpp>

// http://avid.force.com/pkb/KB_Render_FAQ?id=kA031000000P4di&lang=en_US
//   we are using film/movie order
// https://trac.ffmpeg.org/wiki/AudioChannelManipulation#a5.1stereo

namespace tkn {

// util
unsigned resampleCount(unsigned srcRate, unsigned dstRate, unsigned srcn) {
	return std::ceil(srcn * (((double) dstRate) / srcRate));
}

unsigned invResampleCount(unsigned srcRate, unsigned dstRate, unsigned dstn) {
	return std::floor(dstn * (((double) srcRate) / dstRate));
}

// remixing
template<unsigned CSrc, unsigned CDst> struct Downmix;

// src and dst may overlap for all
template<unsigned CSrc> struct Downmix<CSrc, 1> {
	static void apply(float* src, float* dst) {
		float sum = 0.0;
		for(unsigned i = 0u; i < CSrc; ++i) {
			sum += src[i];
		}
		dst[0] = sum / CSrc;
	}
};

template<> struct Downmix<6, 2> {
	static constexpr float lfe = 0.0; // discard
	static constexpr float fl = 0.707f;

	static void apply(float* src, float* dst) {
		dst[0] = src[0] + fl * src[2] + fl * src[3] + lfe * src[5];
		dst[1] = src[1] + fl * src[2] + fl * src[4] + lfe * src[5];
	}
};

template<> struct Downmix<8, 2> {
	static constexpr float lfe = 0.0; // discard
	static constexpr float fl = 0.707f;

	static void apply(float* src, float* dst) {
		dst[0] = src[0] + fl * src[2] + fl * src[3] + fl * src[5] + lfe * src[7];
		dst[1] = src[1] + fl * src[2] + fl * src[4] + fl * src[6] + lfe * src[7];
	}
};

template<> struct Downmix<8, 6> {
	static void apply(float* src, float* dst) {
		// first 3 channels are the same
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];

		dst[3] = src[3] + src[5]; // add l rear to l side
		dst[4] = src[4] + src[6]; // add r rear to r side
		dst[5] = src[7]; // move lfe to channel 5
	}
};

template<unsigned CSrc, unsigned CDst>
void downmix(float* buf, unsigned nf) {
	downmix<CSrc, CDst>(buf, buf, nf);
}

template<unsigned CSrc, unsigned CDst>
void downmix(float* src, float* dst, unsigned nf) {
	for(auto i = 0u; i < nf; ++i) {
		Downmix<CSrc, CDst>::apply(src + i * CSrc, dst + i * CDst);
	}
}

void downmix(float* src, float* dst, unsigned nf, unsigned srcc, unsigned dstc) {
	switch(srcc) {
		case 8:
			switch(dstc) {
				case 1: downmix<8, 1>(src, dst, nf); return;
				case 2: downmix<8, 2>(src, dst, nf); return;
				case 6: downmix<8, 6>(src, dst, nf); return;
				default: break;
			}
			break;
		case 6:
			switch(dstc) {
				case 1: downmix<6, 1>(src, dst, nf); return;
				case 2: downmix<6, 2>(src, dst, nf); return;
				default: break;
			}
			break;
		case 2:
			switch(dstc) {
				case 1: downmix<2, 1>(src, dst, nf); return;
				default: break;
			}
		default: break;
	}

	dlg_error("upmix {} -> {} unimplemented", srcc, dstc);
}

void downmix(float* buf, unsigned nf, unsigned srcc, unsigned dstc) {
	downmix(buf, buf, nf, srcc, dstc);
}

// upmixing
template<unsigned CSrc, unsigned CDst> struct Upmix;

template<unsigned CDst> struct Upmix<1, CDst> {
	static void apply(float* src, float* dst) {
		for(auto i = 0u; i < CDst; ++i) {
			dst[i] = src[0];
		}
	}
};

template<> struct Upmix<2, 6> {
	static constexpr float fl = 0.707f;

	static void apply(float* src, float* dst) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[0] + src[1];
		dst[3] = fl * src[0];
		dst[4] = fl * src[1];
		dst[5] = 0.f;
	}
};

// TODO: no idea about those, really
template<> struct Upmix<2, 8> {
	static constexpr float fl = 0.707f;

	static void apply(float* src, float* dst) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[0] + src[1];
		dst[3] = fl * src[0];
		dst[4] = fl * src[1];
		dst[5] = fl * src[0];
		dst[6] = fl * src[1];
		dst[7] = 0.f;
	}
};

template<> struct Upmix<6, 8> {
	static constexpr float fl = 0.707f;

	static void apply(float* src, float* dst) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		dst[3] = src[3];
		dst[4] = src[4];
		dst[5] = fl * src[3];
		dst[6] = fl * src[4];
		dst[7] = src[5];
	}
};

template<unsigned CSrc, unsigned CDst>
void upmix(float* src, float* dst, unsigned nf) {
	for(auto i = 0u; i < nf; ++i) {
		Upmix<CSrc, CDst>::apply(src + i * CSrc, dst + i * CDst);
	}
}

void upmix(float* src, float* dst, unsigned nf, unsigned srcc, unsigned dstc) {
	switch(srcc) {
		case 1:
			switch(dstc) {
				case 2: upmix<1, 2>(src, dst, nf); return;
				case 6: upmix<1, 6>(src, dst, nf); return;
				case 8: upmix<1, 8>(src, dst, nf); return;
				default: break;
			}
			break;
		case 2:
			switch(dstc) {
				case 6: upmix<2, 6>(src, dst, nf); return;
				case 8: upmix<2, 8>(src, dst, nf); return;
				default: break;
			}
			break;
		case 6:
			switch(dstc) {
				case 8: upmix<6, 8>(src, dst, nf); return;
				default: break;
			}
		default: break;
	}

	dlg_error("upmix {} -> {} unimplemented", srcc, dstc);
}

// resampling
unsigned resample(SoundBufferView dst, SoundBufferView src) {
	int err {};
	auto speex = speex_resampler_init(src.channelCount,
		src.rate, dst.rate, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
	dlg_assertm(speex && !err, "{}", err);
	auto ret = resample(speex, dst, src);
	speex_resampler_destroy(speex);
	return ret;
}

unsigned resample(SpeexResamplerState* speex,
		SoundBufferView dst, SoundBufferView src) {
	dlg_assert(dst.channelCount == src.channelCount);
	dlg_assert(dst.rate != src.rate);

	int err {};
	speex_resampler_set_input_stride(speex, src.channelCount);
	speex_resampler_set_output_stride(speex, dst.channelCount);
	unsigned ret = 0xFFFFFFFFu;
	for(auto i = 0u; i < dst.channelCount; ++i) {
		spx_uint32_t in_len = src.frameCount;
		spx_uint32_t out_len = dst.frameCount;
		err = speex_resampler_process_float(speex, i,
			src.data + i, &in_len, dst.data + i, &out_len);

		dlg_assertm(!err, "{}", err);
		dlg_assertm(in_len == src.frameCount, "{} vs {}",
			in_len, src.frameCount);
		dlg_assertm(out_len == dst.frameCount || out_len == dst.frameCount - 1,
			"{} vs {}", out_len, dst.frameCount);

		if(i == 0u) {
			ret = out_len;
		} else {
			dlg_assert(ret == out_len);
		}
	}

	return ret;
}

UniqueSoundBuffer resample(SoundBufferView view, unsigned rate, unsigned nc) {
	UniqueSoundBuffer ret;
	ret.rate = rate;

	if(rate != view.rate) {
		ret.channelCount = view.channelCount;
		ret.frameCount = resampleCount(view.rate, rate, view.frameCount);
		ret.data = std::make_unique<float[]>(ret.frameCount * ret.channelCount);
		resample(ret, view);
	}

	if(nc > view.channelCount) {
		auto od = std::move(ret.data);
		auto src = od ? od.get() : view.data;
		ret.data = std::make_unique<float[]>(ret.frameCount * nc);
		upmix(src, ret.data.get(), ret.frameCount, view.channelCount, nc);
	} else if(nc < view.channelCount) {
		auto src = ret.data.get();
		if(!ret.data.get()) {
			auto count = ret.frameCount * nc;
			ret.data = std::make_unique<float[]>(count);
			src = view.data;
		}

		downmix(src, ret.data.get(), ret.frameCount, view.channelCount, nc);
	}

	ret.channelCount = nc;
	return ret;
}

// writing
void writeSamples(unsigned nf, float* buf, RingBuffer<float>& rb,
		bool mix, unsigned srcChannels, unsigned dstChannels,
		BufCache& tmpbuf, float volume) {
	dlg_assert(srcChannels <= dstChannels);
	auto dns = dstChannels * nf;
	if(volume == volumePause) {
		if(!mix) {
			std::memset(buf, 0x0, dns * sizeof(float));
		}

		return;
	}

	if(mix) {
		auto sns = nf * srcChannels;
		auto b0 = tmpbuf.get(sns).data();
		auto src = b0;
		auto count = rb.dequeue(b0, sns);

		if(srcChannels < dstChannels) {
			dlg_assert(count % srcChannels == 0);
			count /= srcChannels;
			auto b1 = tmpbuf.get<1>(count * dstChannels).data();
			upmix(b0, b1, count, srcChannels, dstChannels);
			src = b1;
			count *= dstChannels;
		}

		for(auto i = 0u; i < count; ++i) {
			buf[i] += volume * src[i];
		}
	} else if(volume != 1.f || srcChannels < dstChannels) {
		auto sns = nf * srcChannels;
		auto b0 = tmpbuf.get(sns).data();
		auto src = b0;
		auto count = rb.dequeue(b0, sns);

		if(srcChannels < dstChannels) {
			dlg_assert(count % srcChannels == 0);
			count /= srcChannels;
			auto b1 = tmpbuf.get<1>(count * dstChannels).data();
			upmix(b0, b1, count, srcChannels, dstChannels);
			src = b1;
			count *= dstChannels;
		}

		if(volume == 1.f) {
			std::memcpy(buf, src, count * sizeof(float));
		} else {
			for(auto i =0u; i < count; ++i) {
				buf[i] = volume * src[i];
			}
		}

		std::memset(buf + count, 0x0, (dns - count) * sizeof(float));
	} else {
		auto count = rb.dequeue(buf, dns);
		std::memset(buf + count, 0x0, (dns - count) * sizeof(float));
	}
}

} // namespace tkn
