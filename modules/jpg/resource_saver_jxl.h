#ifndef RESOURCE_SAVER_JXL_H
#define RESOURCE_SAVER_JXL_H

#include "core/io/resource_saver.h"

class ResourceFormatSaverJXL : public ResourceFormatSaver {
public:
	virtual Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	virtual bool recognize(const Ref<Resource> &p_resource) const override;
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
};

#endif // RESOURCE_SAVER_JXL_H