#include "font.hpp"
#include "text.hpp"
#include <vpp/vk.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>
#include <nytl/utf.hpp>

#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"

namespace rvg2 {

// FontAtlas
FontAtlas::FontAtlas(UpdateContext& ctx) {
	init(ctx);
}

FontAtlas::~FontAtlas() {
	fonsDeleteInternal(ctx_);
}

void FontAtlas::init(UpdateContext& ctx) {
	dlg_assert(!valid());
	context_ = &ctx;

	FONSparams params {};
	params.flags = FONS_ZERO_TOPLEFT;
	params.width = 512;
	params.height = 512;

	ctx_ = fonsCreateInternal(&params);
	fonsSetAlign(ctx_, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);
}

void FontAtlas::validate() {
	int dirty[4];
	if(fonsValidateTexture(ctx_, dirty)) {
		// TODO: use dirty rect, don't update all in updateDevice
		// TODO: update batcher
		// context().registerUpdateDevice(this);
	}
}

void FontAtlas::expand() {
	constexpr auto maxSize = 4096;

	int w, h;
	fonsGetAtlasSize(ctx_, &w, &h);

	w = std::min(maxSize, w * 2);
	h = std::min(maxSize, h * 2);

	// TODO: instead of using reset and updating all texts we
	// could use ExpandAtlas. Try it and see if the result is much worse
	// (since rectpacking cannot be done again).
	// But even then, we would have to update all Text uvs to match
	// the new size.
	fonsResetAtlas(ctx_, w, h);
	for(auto& t : texts_) {
		dlg_assert(t);
		// TODO(important) re-enable for new text
		// t->update();
	}

	registerDeviceUpdate();
}

void FontAtlas::added(Text& t) {
	dlg_assert(std::find(texts_.begin(), texts_.end(), &t) == texts_.end());
	texts_.push_back(&t);
}

void FontAtlas::removed(Text& t) {
	auto it = std::find(texts_.begin(), texts_.end(), &t);
	dlg_assert(it != texts_.end());
	texts_.erase(it);
}

void FontAtlas::moved(Text& t, Text& tn) noexcept {
	auto it = std::find(texts_.begin(), texts_.end(), &t);
	dlg_assert(it != texts_.end());
	*it = &tn;
}

UpdateFlags FontAtlas::updateDevice() {
	dlg_assert(valid());

	int w, h;
	auto data = fonsGetTextureData(ctx_, &w, &h);
	dlg_assert(w > 0 && h > 0);

	auto fs = Vec2ui {};
	fs.x = w;
	fs.y = h;

	auto dptr = reinterpret_cast<const std::byte*>(data);
	auto dsize = fs.x * fs.y;
	UpdateFlags ret = UpdateFlags::none;
	if(fs != texture_.size()) {
		// TODO: kinda sketchy to call init again, Texture API
		// does not guarantee this works.
		texture_.init(updateContext(), fs, {dptr, dsize}, TextureType::a8);
		ret |= UpdateFlags::rerec;
	} else {
		// NOTE: important to not call texture_.update here since
		//   then the texture would register for updateDevice.
		//   But we currently are in the updateDevice phase, calling
		//   context registerUpdateDevice during that is not allowed
		// TODO: use dirty rect
		ret |= texture_.updateDevice({dptr, dptr + dsize});
	}

	return ret;
}

nytl::Span<std::byte> FontAtlas::addBlob(std::vector<std::byte> blob) {
	blobs_.emplace_back(std::move(blob));
	return blobs_.back();
}

} // namespace rvg2
