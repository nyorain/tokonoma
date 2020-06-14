#include <tkn/image.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

ReadError loadJpeg(std::unique_ptr<Stream>&&, std::unique_ptr<ImageProvider>&) {
	dlg_debug("trigerred dummy readJpeg implementation");
	return ReadError::invalidType;
}

} // namespace tkn
