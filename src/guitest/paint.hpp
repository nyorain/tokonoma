#pragma once

#include "update.hpp"
#include "draw.hpp"
#include <nytl/vec.hpp>
#include <nytl/stringParam.hpp>

namespace rvg2 {

using namespace nytl;

enum class PaintType : std::uint32_t {
	color = 1,
	linGrad = 2,
	radGrad = 3,
	textureRGBA = 4,
	textureA = 5,
	pointColor = 6,
};

PaintData colorPaint(const Vec4f&);
PaintData linearGradient(Vec2f start, Vec2f end,
	const Vec4f& startColor, const Vec4f& endColor);
PaintData radialGradient(Vec2f center, float innerRadius, float outerRadius,
	const Vec4f& innerColor, const Vec4f& outerColor);
PaintData texturePaintRGBA(const nytl::Mat4f& transform);
PaintData texturePaintA(const nytl::Mat4f& transform);
PaintData pointColorPaint();

// - Texture -
/// Type (format) of a texture.
enum class TextureType {
	rgba32,
	a8
};

/// Texture on the device.
class Texture : public DeviceObject {
public:
	using Type = TextureType;

public:
	Texture() = default;
	~Texture() = default;

	// TODO: might be useful
	Texture(Texture&&) noexcept = delete;
	Texture& operator=(Texture&&) noexcept = delete;

	/// See init methods below
	Texture(UpdateContext&, nytl::StringParam filename, Type = Type::rgba32);
	Texture(UpdateContext&, Vec2ui size, nytl::Span<const std::byte> data, Type);

	/// Attempts to load the texture from the given file.
	/// Throws std::runtime_error if the file cannot be loaded.
	/// The passed type will be forced.
	void init(UpdateContext&, nytl::StringParam filename, Type = Type::rgba32);

	/// Creates the texture with given size, data and type.
	/// data must reference at least size.x * size.y * formatSize(type) bytes,
	/// where formatSize(Type::a8) = 1 and formatSize(Type::rgba32) = 4.
	void init(UpdateContext&, Vec2ui size, nytl::Span<const std::byte> data, Type);


	/// Updates the given texture with the given data.
	/// data must reference at least size.x * size.y * formatSize(type()) bytes,
	/// where formatSize(Type::a8) = 1 and formatSize(Type::rgba32) = 4.
	void update(std::vector<std::byte> data);

	const auto& viewableImage() const { return image_; }
	const auto& size() const { return size_; }
	auto vkImage() const { return viewableImage().vkImage(); }
	auto vkImageView() const { return viewableImage().vkImageView(); }
	auto type() const { return type_; }

	UpdateFlags updateDevice(std::vector<std::byte> data);
	UpdateFlags updateDevice() override;

protected:
	void create();
	void upload(nytl::Span<const std::byte> data, vk::ImageLayout);

protected:
	vpp::ViewableImage image_;
	Vec2ui size_ {};
	Type type_ {};
	std::vector<std::byte> pending_;
};

} // namespace rvg2
