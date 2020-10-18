#pragma once

#include "update.hpp"
#include "draw.hpp"
#include "paint.hpp"

#include <vpp/trackedDescriptor.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/rect.hpp>

struct FONScontext;

namespace rvg2 {

class Text;

// TODO: currently fonts cannot be removed from a font atlas.

/// Holds a texture on the device to which multiple fonts can be uploaded.
/// See the Font class for FontAtlas restrictions and grouping.
class FontAtlas : public DeviceObject {
public:
	FontAtlas() = default;
	FontAtlas(UpdateContext&);
	~FontAtlas();

	FontAtlas(FontAtlas&&) = delete;
	FontAtlas& operator=(FontAtlas&&) = delete;

	void init(UpdateContext& ctx);

	auto& texture() const { return texture_; }
	auto* stash() const { return ctx_; }

	// - usually not needed manually -
	UpdateFlags updateDevice() override;
	void validate();
	void expand();
	nytl::Span<std::byte> addBlob(std::vector<std::byte>);

	void added(Text&);
	void removed(Text&);
	void moved(Text&, Text&) noexcept;

protected:
	FONScontext* ctx_ {};
	std::vector<Text*> texts_;
	Texture texture_;
	std::vector<std::vector<std::byte>> blobs_;
};

/// Represents information about one font in a font atlas.
/// Lightweight discription, all actual data is stored in the font
/// atlas. Therefore can be copied without overhead.
/// Fonts cannot be removed from a font atlas again; in this case the
/// whole font atlas has to be destroyed. One can only add fallbacks
/// fonts from the same font atlas. The more fonts are in a font atlas,
/// the slower an update is (e.g. a newly added glyph).
/// Therefore fonts should be grouped efficiently together in font atlases.
class FontRef {
public:
	struct Metrics {
		float lineHeight;
		float ascender;
		float descender;
	};

public:
	FontRef() = default;

	/// Loads the font from a given file.
	/// Throws on error (e.g. if the file does not exist/invalid font).
	/// The first overload uses the contexts default font atlas.
	FontRef(Context&, StringParam file);
	FontRef(FontAtlas&, StringParam file);

	/// Loads the font from a otf/ttf blob.
	/// Throws on error (invalid font data).
	/// The first overload uses the contexts default font atlas.
	FontRef(Context&, std::vector<std::byte> data);
	FontRef(FontAtlas&, std::vector<std::byte> data);

	/// Create a pure logical font for the given atlas (or default atlas).
	/// A pure logical font can only be used with fallback fonts.
	FontRef(Context&);
	FontRef(FontAtlas&);

	/// Adds the given font as fallback.
	/// Must be allocated on the same atlas.
	void fallback(const FontRef& f);

	float width(std::string_view text, unsigned height) const;
	nytl::Rect2f bounds(std::string_view text, unsigned height) const;
	Metrics metrics() const;

	bool valid() const { return atlas_ && id_ >= 0; }
	auto& atlas() const { return *atlas_; }
	auto id() const { return id_; }

protected:
	FontAtlas* atlas_ {};
	int id_ {-1};
};

} // namespace rvg
