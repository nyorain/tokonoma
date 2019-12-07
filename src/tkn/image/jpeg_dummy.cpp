#include <tkn/image.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

ReadError readJpeg(nytl::StringParam, std::unique_ptr<ImageProvider>&) {
	dlg_debug("trigerred dummy readJpeg implementation");
	return ReadError::invalidType;
}

ReadError readJpeg(File&&, std::unique_ptr<ImageProvider>&) {
	dlg_debug("trigerred dummy readJpeg implementation");
	return ReadError::invalidType;
}

} // namespace tkn
