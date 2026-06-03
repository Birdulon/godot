#include "resource_saver_jxl.h"

#include "core/io/image.h"

Error ResourceFormatSaverJXL::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<Image> image = p_resource;

	if (image.is_null()) {
		// Try to extract the image if the resource is a Texture2D (like ImageTexture).
		if (p_resource->has_method("get_image")) {
			image = p_resource->call("get_image");
		}
	}

	ERR_FAIL_COND_V_MSG(image.is_null(), ERR_INVALID_PARAMETER, "Can't save resource as JXL because it's not an Image or Texture2D.");

	// Save the image to JXL format (defaulting to quality 1.0f / lossless)
	return image->save_jxl(p_path, 1.0f);
}

bool ResourceFormatSaverJXL::recognize(const Ref<Resource> &p_resource) const {
	return p_resource->is_class("Image") || p_resource->is_class("Texture2D");
}

void ResourceFormatSaverJXL::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (recognize(p_resource)) {
		p_extensions->push_back("jxl");
	}
}