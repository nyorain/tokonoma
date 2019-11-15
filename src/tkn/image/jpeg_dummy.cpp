#include <tkn/image.hpp>

namespace tkn {

ReadError readJpeg(nytl::StringParam, std::unique_ptr<ImageProvider>&) {
	return ReadError::invalidType;
}

} // namespace tkn
