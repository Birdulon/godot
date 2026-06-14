/**************************************************************************/
/*  image_loader_libjxl.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "image_loader_libjxl.h"

#include <jxl/decode.h>
#include <jxl/encode.h>

// Attempts to decode a JPEG XL (or JPEG) image from a buffer using libjxl.
// For JPEG files, this internally transcodes JPEG -> JXL -> pixels using
// JxlEncoderAddJPEGFrame so that no external JPEG library is needed.
Error jxl_load_image_from_buffer(Image *p_image, const uint8_t *p_buffer, int p_buffer_len) {
	// --- Phase 1: Try direct JPEG XL decoding ---
	{
		JxlDecoder *dec = JxlDecoderCreate(nullptr);
		if (dec == nullptr) {
			return FAILED;
		}

		if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
			JxlDecoderDestroy(dec);
			return ERR_FILE_CORRUPT;
		}

		if (JxlDecoderSetInput(dec, p_buffer, p_buffer_len) != JXL_DEC_SUCCESS) {
			JxlDecoderDestroy(dec);
			return ERR_FILE_CORRUPT;
		}

		JxlBasicInfo info;
		JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };

		Vector<uint8_t> data;
		Image::Format gd_pixel_format = Image::FORMAT_RGBA8;

		bool jxl_success = false;

		for (;;) {
			JxlDecoderStatus status = JxlDecoderProcessInput(dec);
			if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
				// Not a valid JXL file (or more input needed).
				break;
			} else if (status == JXL_DEC_BASIC_INFO) {
				if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
					break;
				}

				bool is_float = info.exponent_bits_per_sample > 0;
				bool is_16bit = info.bits_per_sample > 8;

				if (is_float) {
					format.data_type = JXL_TYPE_FLOAT;
					if (info.alpha_bits == 0 && info.num_color_channels == 1) {
						format.num_channels = 1;
						gd_pixel_format = Image::FORMAT_RF;
					} else if (info.alpha_bits == 0 && info.num_color_channels == 3) {
						format.num_channels = 3;
						gd_pixel_format = Image::FORMAT_RGBF;
					} else if (info.alpha_bits > 0 && info.num_color_channels == 1) {
						format.num_channels = 2;
						gd_pixel_format = Image::FORMAT_RGF;
					} else {
						format.num_channels = 4;
						gd_pixel_format = Image::FORMAT_RGBAF;
					}
				} else if (is_16bit) {
					format.data_type = JXL_TYPE_UINT16;
					if (info.alpha_bits == 0 && info.num_color_channels == 1) {
						format.num_channels = 1;
						gd_pixel_format = Image::FORMAT_R16;
					} else if (info.alpha_bits == 0 && info.num_color_channels == 3) {
						format.num_channels = 3;
						gd_pixel_format = Image::FORMAT_RGB16;
					} else if (info.alpha_bits > 0 && info.num_color_channels == 1) {
						format.num_channels = 2;
						gd_pixel_format = Image::FORMAT_RG16;
					} else {
						format.num_channels = 4;
						gd_pixel_format = Image::FORMAT_RGBA16;
					}
				} else {
					format.data_type = JXL_TYPE_UINT8;
					if (info.alpha_bits == 0 && info.num_color_channels == 1) {
						format.num_channels = 1;
						gd_pixel_format = Image::FORMAT_L8;
					} else if (info.alpha_bits == 0 && info.num_color_channels == 3) {
						format.num_channels = 3;
						gd_pixel_format = Image::FORMAT_RGB8;
					} else if (info.alpha_bits > 0 && info.num_color_channels == 1) {
						format.num_channels = 2;
						gd_pixel_format = Image::FORMAT_LA8;
					} else {
						format.num_channels = 4;
						gd_pixel_format = Image::FORMAT_RGBA8;
					}
				}
			} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
				size_t buffer_size;
				if (JxlDecoderImageOutBufferSize(dec, &format, &buffer_size) != JXL_DEC_SUCCESS) {
					break;
				}
				data.resize(buffer_size);
				if (JxlDecoderSetImageOutBuffer(dec, &format, data.ptrw(), buffer_size) != JXL_DEC_SUCCESS) {
					break;
				}
			} else if (status == JXL_DEC_SUCCESS) {
				jxl_success = true;
				break;
			}
		}

		if (jxl_success) {
			JxlDecoderDestroy(dec);
			p_image->set_data(info.xsize, info.ysize, false, gd_pixel_format, data);
			return OK;
		}

		JxlDecoderDestroy(dec);
	}

	// --- Phase 2: If the data looks like a regular JPEG, transcode it through JXL ---
	// libjxl's JxlDecoder does not natively parse plain JPEG files (they start
	// with 0xFF 0xD8 0xFF, not the JXL codestream marker 0xFF 0x0A). However,
	// JxlEncoderAddJPEGFrame can take raw JPEG bytes, parse them internally,
	// and produce a valid JXL container. We then decode that container back to
	// pixels -- all through libjxl without involving libturbo-jpeg.
	if (p_buffer_len >= 3 && p_buffer[0] == 0xFF && p_buffer[1] == 0xD8 && p_buffer[2] == 0xFF) {
		// --- Step A: Encode JPEG -> JXL in memory ---
		JxlEncoder *enc = JxlEncoderCreate(nullptr);
		if (enc == nullptr) {
			return FAILED;
		}

		JxlEncoderUseContainer(enc, JXL_TRUE);

		JxlEncoderFrameSettings *frame_settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
		if (!frame_settings) {
			JxlEncoderDestroy(enc);
			return FAILED;
		}

		// Losslessly store the decoded JPEG pixels in the JXL codestream.
		JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE);

		if (JxlEncoderAddJPEGFrame(frame_settings, p_buffer, p_buffer_len) != JXL_ENC_SUCCESS) {
			JxlEncoderDestroy(enc);
			return FAILED;
		}

		JxlEncoderCloseInput(enc);

		Vector<uint8_t> jxl_data;
		jxl_data.resize(65536);
		uint8_t *next_out = jxl_data.ptrw();
		size_t avail_out = jxl_data.size();

		JxlEncoderStatus process_result;
		while ((process_result = JxlEncoderProcessOutput(enc, &next_out, &avail_out)) == JXL_ENC_NEED_MORE_OUTPUT) {
			size_t offset = next_out - jxl_data.ptrw();
			jxl_data.resize(jxl_data.size() * 2);
			next_out = jxl_data.ptrw() + offset;
			avail_out = jxl_data.size() - offset;
		}

		if (process_result != JXL_ENC_SUCCESS) {
			JxlEncoderDestroy(enc);
			return FAILED;
		}

		jxl_data.resize(next_out - jxl_data.ptrw());
		JxlEncoderDestroy(enc);

		// --- Step B: Decode the generated JXL data back to pixels ---
		JxlDecoder *dec = JxlDecoderCreate(nullptr);
		if (dec == nullptr) {
			return FAILED;
		}

		if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
			JxlDecoderDestroy(dec);
			return ERR_FILE_CORRUPT;
		}

		if (JxlDecoderSetInput(dec, jxl_data.ptr(), jxl_data.size()) != JXL_DEC_SUCCESS) {
			JxlDecoderDestroy(dec);
			return ERR_FILE_CORRUPT;
		}

		JxlBasicInfo info;
		JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };

		Vector<uint8_t> data;
		Image::Format gd_pixel_format = Image::FORMAT_RGBA8;

		for (;;) {
			JxlDecoderStatus status = JxlDecoderProcessInput(dec);
			if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
				JxlDecoderDestroy(dec);
				return ERR_FILE_CORRUPT;
			} else if (status == JXL_DEC_BASIC_INFO) {
				if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
					JxlDecoderDestroy(dec);
					return ERR_FILE_CORRUPT;
				}

				bool is_float = info.exponent_bits_per_sample > 0;
				bool is_16bit = info.bits_per_sample > 8;

				if (is_float) {
					format.data_type = JXL_TYPE_FLOAT;
					if (info.alpha_bits == 0 && info.num_color_channels == 1) {
						format.num_channels = 1;
						gd_pixel_format = Image::FORMAT_RF;
					} else if (info.alpha_bits == 0 && info.num_color_channels == 3) {
						format.num_channels = 3;
						gd_pixel_format = Image::FORMAT_RGBF;
					} else if (info.alpha_bits > 0 && info.num_color_channels == 1) {
						format.num_channels = 2;
						gd_pixel_format = Image::FORMAT_RGF;
					} else {
						format.num_channels = 4;
						gd_pixel_format = Image::FORMAT_RGBAF;
					}
				} else if (is_16bit) {
					format.data_type = JXL_TYPE_UINT16;
					if (info.alpha_bits == 0 && info.num_color_channels == 1) {
						format.num_channels = 1;
						gd_pixel_format = Image::FORMAT_R16;
					} else if (info.alpha_bits == 0 && info.num_color_channels == 3) {
						format.num_channels = 3;
						gd_pixel_format = Image::FORMAT_RGB16;
					} else if (info.alpha_bits > 0 && info.num_color_channels == 1) {
						format.num_channels = 2;
						gd_pixel_format = Image::FORMAT_RG16;
					} else {
						format.num_channels = 4;
						gd_pixel_format = Image::FORMAT_RGBA16;
					}
				} else {
					format.data_type = JXL_TYPE_UINT8;
					if (info.alpha_bits == 0 && info.num_color_channels == 1) {
						format.num_channels = 1;
						gd_pixel_format = Image::FORMAT_L8;
					} else if (info.alpha_bits == 0 && info.num_color_channels == 3) {
						format.num_channels = 3;
						gd_pixel_format = Image::FORMAT_RGB8;
					} else if (info.alpha_bits > 0 && info.num_color_channels == 1) {
						format.num_channels = 2;
						gd_pixel_format = Image::FORMAT_LA8;
					} else {
						format.num_channels = 4;
						gd_pixel_format = Image::FORMAT_RGBA8;
					}
				}
			} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
				size_t buffer_size;
				if (JxlDecoderImageOutBufferSize(dec, &format, &buffer_size) != JXL_DEC_SUCCESS) {
					JxlDecoderDestroy(dec);
					return ERR_FILE_CORRUPT;
				}
				data.resize(buffer_size);
				if (JxlDecoderSetImageOutBuffer(dec, &format, data.ptrw(), buffer_size) != JXL_DEC_SUCCESS) {
					JxlDecoderDestroy(dec);
					return ERR_FILE_CORRUPT;
				}
			} else if (status == JXL_DEC_SUCCESS) {
				break;
			}
		}

		JxlDecoderDestroy(dec);
		p_image->set_data(info.xsize, info.ysize, false, gd_pixel_format, data);
		return OK;
	}

	// Not a recognized JXL or JPEG file.
	return ERR_FILE_CORRUPT;
}



Error ImageLoaderLibJXL::load_image(Ref<Image> p_image, Ref<FileAccess> f, BitField<ImageFormatLoader::LoaderFlags> p_flags, float p_scale) {
	Vector<uint8_t> src_image;
	uint64_t src_image_len = f->get_length();
	ERR_FAIL_COND_V(src_image_len == 0, ERR_FILE_CORRUPT);
	src_image.resize(src_image_len);

	uint8_t *w = src_image.ptrw();

	f->get_buffer(&w[0], src_image_len);

	Error err = jxl_load_image_from_buffer(p_image.ptr(), w, src_image_len);

	return err;
}

void ImageLoaderLibJXL::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("jxl");
	// Also register for JPEG files so this loader can be used as an
	// alternative to libturbo-jpeg (the JPEG -> JXL -> pixels roundtrip
	// is handled internally by jxl_load_image_from_buffer).
	p_extensions->push_back("jpg");
	p_extensions->push_back("jpeg");
}

static Ref<Image> _jxl_mem_loader_func(const uint8_t *p_data, int p_size) {
	Ref<Image> img;
	img.instantiate();
	Error err = jxl_load_image_from_buffer(img.ptr(), p_data, p_size);
	ERR_FAIL_COND_V(err, Ref<Image>());
	return img;
}

static Vector<uint8_t> _jxl_buffer_save_func(const Ref<Image> &p_img, float p_quality) {
	Vector<uint8_t> output;

	ERR_FAIL_COND_V(p_img.is_null() || p_img->is_empty(), output);

	Ref<Image> image = p_img->duplicate();
	if (image->is_compressed()) {
		Error error = image->decompress();
		ERR_FAIL_COND_V_MSG(error != OK, output, "Couldn't decompress image.");
	}

	Image::Format gd_format = image->get_format();
	if (gd_format != Image::FORMAT_L8 && gd_format != Image::FORMAT_LA8 &&
			gd_format != Image::FORMAT_R8 && gd_format != Image::FORMAT_RG8 &&
			gd_format != Image::FORMAT_RGB8 && gd_format != Image::FORMAT_RGBA8 &&
			gd_format != Image::FORMAT_R16 && gd_format != Image::FORMAT_RG16 &&
			gd_format != Image::FORMAT_RGB16 && gd_format != Image::FORMAT_RGBA16 &&
			gd_format != Image::FORMAT_RF && gd_format != Image::FORMAT_RGF &&
			gd_format != Image::FORMAT_RGBF && gd_format != Image::FORMAT_RGBAF) {
		image->convert(Image::FORMAT_RGBA8);
		gd_format = Image::FORMAT_RGBA8;
	}

	JxlEncoder *enc = JxlEncoderCreate(nullptr);
	ERR_FAIL_NULL_V_MSG(enc, output, "Couldn't create JxlEncoder");

	JxlEncoderUseContainer(enc, JXL_TRUE);
	JxlBasicInfo info;
	JxlEncoderInitBasicInfo(&info);
	info.xsize = image->get_width();
	info.ysize = image->get_height();
	info.uses_original_profile = JXL_FALSE;

	JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };

	if (gd_format == Image::FORMAT_RF || gd_format == Image::FORMAT_RGF || gd_format == Image::FORMAT_RGBF || gd_format == Image::FORMAT_RGBAF) {
		info.bits_per_sample = 32;
		info.exponent_bits_per_sample = 8;
		format.data_type = JXL_TYPE_FLOAT;
	} else if (gd_format == Image::FORMAT_R16 || gd_format == Image::FORMAT_RG16 || gd_format == Image::FORMAT_RGB16 || gd_format == Image::FORMAT_RGBA16) {
		info.bits_per_sample = 16;
		info.exponent_bits_per_sample = 0;
		format.data_type = JXL_TYPE_UINT16;
	} else {
		info.bits_per_sample = 8;
		info.exponent_bits_per_sample = 0;
		format.data_type = JXL_TYPE_UINT8;
	}

	if (gd_format == Image::FORMAT_L8 || gd_format == Image::FORMAT_R8 || gd_format == Image::FORMAT_R16 || gd_format == Image::FORMAT_RF) {
		info.num_color_channels = 1;
		info.num_extra_channels = 0;
		info.alpha_bits = 0;
		format.num_channels = 1;
	} else if (gd_format == Image::FORMAT_LA8 || gd_format == Image::FORMAT_RG8 || gd_format == Image::FORMAT_RG16 || gd_format == Image::FORMAT_RGF) {
		info.num_color_channels = 1;
		info.num_extra_channels = 1;
		info.alpha_bits = info.bits_per_sample;
		format.num_channels = 2;
	} else if (gd_format == Image::FORMAT_RGB8 || gd_format == Image::FORMAT_RGB16 || gd_format == Image::FORMAT_RGBF) {
		info.num_color_channels = 3;
		info.num_extra_channels = 0;
		info.alpha_bits = 0;
		format.num_channels = 3;
	} else {
		info.num_color_channels = 3;
		info.num_extra_channels = 1;
		info.alpha_bits = info.bits_per_sample;
		format.num_channels = 4;
	}

	JxlEncoderSetBasicInfo(enc, &info);
	JxlColorEncoding color_encoding;
	if (format.data_type == JXL_TYPE_FLOAT || format.data_type == JXL_TYPE_UINT16) {
		JxlColorEncodingSetToLinearSRGB(&color_encoding, info.num_color_channels == 1);
	} else {
		JxlColorEncodingSetToSRGB(&color_encoding, info.num_color_channels == 1);
	}
	JxlEncoderSetColorEncoding(enc, &color_encoding);

	JxlEncoderFrameSettings *frame_settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
	float distance = JxlEncoderDistanceFromQuality(p_quality * 100);
	JxlEncoderSetFrameDistance(frame_settings, distance);
	if (p_quality == 1.0f) {
		JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE);
	}

	if (JxlEncoderAddImageFrame(frame_settings, &format, image->get_data().ptr(), image->get_data().size()) != JXL_ENC_SUCCESS) {
		JxlEncoderDestroy(enc);
		ERR_FAIL_V_MSG(output, "Couldn't compress jxl");
	}

	JxlEncoderCloseInput(enc);

	output.resize(65536);
	uint8_t *next_out = output.ptrw();
	size_t avail_out = output.size();

	JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;
	while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
		process_result = JxlEncoderProcessOutput(enc, &next_out, &avail_out);
		if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
			size_t offset = next_out - output.ptrw();
			output.resize(output.size() * 2);
			next_out = output.ptrw() + offset;
			avail_out = output.size() - offset;
		}
	}
	output.resize(next_out - output.ptrw());
	JxlEncoderDestroy(enc);

	return output;
}

static Error _jxl_save_func(const String &p_path, const Ref<Image> &p_img, float p_quality) {
	Error err;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V_MSG(err, err, vformat("Can't save JXL at path: '%s'.", p_path));

	Vector<uint8_t> data = _jxl_buffer_save_func(p_img, p_quality);
	ERR_FAIL_COND_V(data.size() == 0, FAILED);
	ERR_FAIL_COND_V_MSG(!file->store_buffer(data.ptr(), data.size()), FAILED, "Failed writing jxl to file");

	return OK;
}

ImageLoaderLibJXL::ImageLoaderLibJXL() {
	Image::_jxl_mem_loader_func = _jxl_mem_loader_func;
	Image::save_jxl_func = _jxl_save_func;
	Image::save_jxl_buffer_func = _jxl_buffer_save_func;
}
