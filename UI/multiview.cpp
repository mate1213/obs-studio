#include "multiview.hpp"
#include "window-basic-main.hpp"
#include "obs-app.hpp"
#include "platform.hpp"
#include "display-helpers.hpp"
#include "obs-audio-controls.h"
#include "obs.h"
#include "obs-source.h"

#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
Multiview::Multiview()
{
	InitSafeAreas(&actionSafeMargin, &graphicsSafeMargin,
		      &fourByThreeSafeMargin, &leftLine, &topLine, &rightLine);
	InitAudioMeter();
}

Multiview::~Multiview()
{
	for (OBSWeakSource &weakSrc : multiviewScenes) {
		OBSSource src = OBSGetStrongRef(weakSrc);
		if (src)
			obs_source_dec_showing(src);
	}

	obs_enter_graphics();
	gs_vertexbuffer_destroy(actionSafeMargin);
	gs_vertexbuffer_destroy(graphicsSafeMargin);
	gs_vertexbuffer_destroy(fourByThreeSafeMargin);
	gs_vertexbuffer_destroy(leftLine);
	gs_vertexbuffer_destroy(topLine);
	gs_vertexbuffer_destroy(rightLine);
	obs_leave_graphics();
}

static OBSSource CreateLabel(const char *name, size_t h)
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();

	std::string text;
	text += " ";
	text += name;
	text += " ";

#if defined(_WIN32)
	obs_data_set_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "Helvetica");
#else
	obs_data_set_string(font, "face", "Monospace");
#endif
	obs_data_set_int(font, "flags", 1); // Bold text
	obs_data_set_int(font, "size", int(h / 9.81));

	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text.c_str());
	obs_data_set_bool(settings, "outline", false);

#ifdef _WIN32
	const char *text_source_id = "text_gdiplus";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	OBSSourceAutoRelease txtSource =
		obs_source_create_private(text_source_id, name, settings);

	return txtSource.Get();
}

void Multiview::Update(MultiviewLayout multiviewLayout, bool drawLabel,
		       bool drawSafeArea, bool drawAudioMeter)
{
	this->multiviewLayout = multiviewLayout;
	this->drawLabel = drawLabel;
	this->drawSafeArea = drawSafeArea;
	this->drawAudioMeter = drawAudioMeter;

	multiviewScenes.clear();
	multiviewLabels.clear();

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	uint32_t w = ovi.base_width;
	uint32_t h = ovi.base_height;
	fw = float(w);
	fh = float(h);
	ratio = fw / fh;

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	multiviewLabels.emplace_back(
		CreateLabel(Str("StudioMode.Preview"), h / 2));
	multiviewLabels.emplace_back(
		CreateLabel(Str("StudioMode.Program"), h / 2));

	switch (multiviewLayout) {
	case MultiviewLayout::HORIZONTAL_TOP_18_SCENES:
		pvwprgCX = fw / 2;
		pvwprgCY = fh / 2;

		maxSrcs = 18;
		break;
	case MultiviewLayout::HORIZONTAL_TOP_24_SCENES:
		pvwprgCX = fw / 3;
		pvwprgCY = fh / 3;

		maxSrcs = 24;
		break;
	case MultiviewLayout::SCENES_ONLY_4_SCENES:
		pvwprgCX = fw / 2;
		pvwprgCY = fh / 2;
		maxSrcs = 4;
		break;
	case MultiviewLayout::SCENES_ONLY_9_SCENES:
		pvwprgCX = fw / 3;
		pvwprgCY = fh / 3;
		maxSrcs = 9;
		break;
	case MultiviewLayout::SCENES_ONLY_16_SCENES:
		pvwprgCX = fw / 4;
		pvwprgCY = fh / 4;
		maxSrcs = 16;
		break;
	case MultiviewLayout::SCENES_ONLY_25_SCENES:
		pvwprgCX = fw / 5;
		pvwprgCY = fh / 5;
		maxSrcs = 25;
		break;
	default:
		pvwprgCX = fw / 2;
		pvwprgCY = fh / 2;

		maxSrcs = 8;
	}

	ppiCX = pvwprgCX - thicknessx2;
	ppiCY = pvwprgCY - thicknessx2;
	ppiScaleX = (pvwprgCX - thicknessx2) / fw;
	ppiScaleY = (pvwprgCY - thicknessx2) / fh;

	switch (multiviewLayout) {
	case MultiviewLayout::HORIZONTAL_TOP_18_SCENES:
		scenesCX = pvwprgCX / 3;
		scenesCY = pvwprgCY / 3;
		break;
	case MultiviewLayout::SCENES_ONLY_4_SCENES:
	case MultiviewLayout::SCENES_ONLY_9_SCENES:
	case MultiviewLayout::SCENES_ONLY_16_SCENES:
	case MultiviewLayout::SCENES_ONLY_25_SCENES:
		scenesCX = pvwprgCX;
		scenesCY = pvwprgCY;
		break;
	default:
		scenesCX = pvwprgCX / 2;
		scenesCY = pvwprgCY / 2;
	}

	siCX = scenesCX - thicknessx2;
	siCY = scenesCY - thicknessx2;
	siScaleX = (scenesCX - thicknessx2) / fw;
	siScaleY = (scenesCY - thicknessx2) / fh;

	numSrcs = 0;
	size_t i = 0;
	while (i < scenes.sources.num && numSrcs < maxSrcs) {
		obs_source_t *src = scenes.sources.array[i++];
		OBSDataAutoRelease data = obs_source_get_private_settings(src);

		obs_data_set_default_bool(data, "show_in_multiview", true);
		if (!obs_data_get_bool(data, "show_in_multiview"))
			continue;

		// We have a displayable source.
		numSrcs++;

		multiviewScenes.emplace_back(OBSGetWeakRef(src));
		obs_source_inc_showing(src);

		multiviewLabels.emplace_back(
			CreateLabel(obs_source_get_name(src), h / 3));
	}

	obs_frontend_source_list_free(&scenes);

	//Create Scale label
	//-------------------
	if (drawAudioMeter) {
		for (int deciBells = 0; deciBells >= minimumLevel;
		     deciBells -= 5) {
			char integer_string[4];

			sprintf(integer_string, "%d", deciBells);
			multiviewLabels.emplace_back(
				CreateLabel(integer_string, h / 10));
		}
	}
}

static inline uint32_t labelOffset(MultiviewLayout multiviewLayout,
				   obs_source_t *label, uint32_t cx)
{
	uint32_t w = obs_source_get_width(label);

	int n; // Twice of scale factor of preview and program scenes
	switch (multiviewLayout) {
	case MultiviewLayout::HORIZONTAL_TOP_24_SCENES:
		n = 6;
		break;
	case MultiviewLayout::SCENES_ONLY_25_SCENES:
		n = 10;
		break;
	case MultiviewLayout::SCENES_ONLY_16_SCENES:
		n = 8;
		break;
	case MultiviewLayout::SCENES_ONLY_9_SCENES:
		n = 6;
		break;
	case MultiviewLayout::SCENES_ONLY_4_SCENES:
		n = 4;
		break;
	default:
		n = 4;
		break;
	}

	w = uint32_t(w * ((1.0f) / n));
	return (cx / 2) - w;
}

void Multiview::Render(uint32_t cx, uint32_t cy)
{
	OBSBasic *main = (OBSBasic *)obs_frontend_get_main_window();

	uint32_t targetCX, targetCY;
	int x, y;
	float scale;

	targetCX = (uint32_t)fw;
	targetCY = (uint32_t)fh;

	GetScaleAndCenterPos(targetCX, targetCY, cx, cy, x, y, scale);

	OBSSource previewSrc = main->GetCurrentSceneSource();
	OBSSource programSrc = main->GetProgramSource();
	bool studioMode = main->IsPreviewProgramMode();

	auto drawBox = [&](float cx, float cy, uint32_t colorVal) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color =
			gs_effect_get_param_by_name(solid, "color");

		gs_effect_set_color(color, colorVal);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, (uint32_t)cx, (uint32_t)cy);
	};

	auto setRegion = [&](float bx, float by, float cx, float cy) {
		float vX = int(x + bx * scale);
		float vY = int(y + by * scale);
		float vCX = int(cx * scale);
		float vCY = int(cy * scale);

		float oL = bx;
		float oT = by;
		float oR = (bx + cx);
		float oB = (by + cy);

		startRegion(vX, vY, vCX, vCY, oL, oR, oT, oB);
	};

	auto calcBaseSource = [&](size_t i) {
		switch (multiviewLayout) {
		case MultiviewLayout::HORIZONTAL_TOP_18_SCENES:
			sourceX = (i % 6) * scenesCX;
			sourceY = pvwprgCY + (i / 6) * scenesCY;
			break;
		case MultiviewLayout::HORIZONTAL_TOP_24_SCENES:
			sourceX = (i % 6) * scenesCX;
			sourceY = pvwprgCY + (i / 6) * scenesCY;
			break;
		case MultiviewLayout::VERTICAL_LEFT_8_SCENES:
			sourceX = pvwprgCX;
			sourceY = (i / 2) * scenesCY;
			if (i % 2 != 0)
				sourceX += scenesCX;
			break;
		case MultiviewLayout::VERTICAL_RIGHT_8_SCENES:
			sourceX = 0;
			sourceY = (i / 2) * scenesCY;
			if (i % 2 != 0)
				sourceX = scenesCX;
			break;
		case MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES:
			if (i < 4) {
				sourceX = (float(i) * scenesCX);
				sourceY = 0;
			} else {
				sourceX = (float(i - 4) * scenesCX);
				sourceY = scenesCY;
			}
			break;
		case MultiviewLayout::SCENES_ONLY_4_SCENES:
			sourceX = (i % 2) * scenesCX;
			sourceY = (i / 2) * scenesCY;
			break;
		case MultiviewLayout::SCENES_ONLY_9_SCENES:
			sourceX = (i % 3) * scenesCX;
			sourceY = (i / 3) * scenesCY;
			break;
		case MultiviewLayout::SCENES_ONLY_16_SCENES:
			sourceX = (i % 4) * scenesCX;
			sourceY = (i / 4) * scenesCY;
			break;
		case MultiviewLayout::SCENES_ONLY_25_SCENES:
			sourceX = (i % 5) * scenesCX;
			sourceY = (i / 5) * scenesCY;
			break;
		default: // MultiviewLayout::HORIZONTAL_TOP_8_SCENES:
			if (i < 4) {
				sourceX = (float(i) * scenesCX);
				sourceY = pvwprgCY;
			} else {
				sourceX = (float(i - 4) * scenesCX);
				sourceY = pvwprgCY + scenesCY;
			}
		}
		siX = sourceX + thickness;
		siY = sourceY + thickness;
	};

	auto calcPreviewProgram = [&](bool program) {
		switch (multiviewLayout) {
		case MultiviewLayout::HORIZONTAL_TOP_24_SCENES:
			sourceX = thickness + pvwprgCX / 2;
			sourceY = thickness;
			labelX = offset + pvwprgCX / 2;
			labelY = pvwprgCY * 0.85f;
			if (program) {
				sourceX += pvwprgCX;
				labelX += pvwprgCX;
			}
			break;
		case MultiviewLayout::VERTICAL_LEFT_8_SCENES:
			sourceX = thickness;
			sourceY = pvwprgCY + thickness;
			labelX = offset;
			labelY = pvwprgCY * 1.85f;
			if (program) {
				sourceY = thickness;
				labelY = pvwprgCY * 0.85f;
			}
			break;
		case MultiviewLayout::VERTICAL_RIGHT_8_SCENES:
			sourceX = pvwprgCX + thickness;
			sourceY = pvwprgCY + thickness;
			labelX = pvwprgCX + offset;
			labelY = pvwprgCY * 1.85f;
			if (program) {
				sourceY = thickness;
				labelY = pvwprgCY * 0.85f;
			}
			break;
		case MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES:
			sourceX = thickness;
			sourceY = pvwprgCY + thickness;
			labelX = offset;
			labelY = pvwprgCY * 1.85f;
			if (program) {
				sourceX += pvwprgCX;
				labelX += pvwprgCX;
			}
			break;
		case MultiviewLayout::SCENES_ONLY_4_SCENES:
		case MultiviewLayout::SCENES_ONLY_9_SCENES:
		case MultiviewLayout::SCENES_ONLY_16_SCENES:
			sourceX = thickness;
			sourceY = thickness;
			labelX = offset;
			break;
		default: // MultiviewLayout::HORIZONTAL_TOP_8_SCENES and 18_SCENES
			sourceX = thickness;
			sourceY = thickness;
			labelX = offset;
			labelY = pvwprgCY * 0.85f;
			if (program) {
				sourceX += pvwprgCX;
				labelX += pvwprgCX;
			}
		}
	};

	auto paintAreaWithColor = [&](float tx, float ty, float cx, float cy,
				      uint32_t color) {
		gs_matrix_push();
		gs_matrix_translate3f(tx, ty, 0.0f);
		drawBox(cx, cy, color);
		gs_matrix_pop();
	};

	// Define the whole usable region for the multiview
	startRegion(x, y, targetCX * scale, targetCY * scale, 0.0f, fw, 0.0f,
		    fh);

	// Change the background color to highlight all sources
	drawBox(fw, fh, outerColor);

	/* ----------------------------- */
	/* draw sources                  */

	for (size_t i = 0; i < maxSrcs; i++) {
		// Handle all the offsets
		calcBaseSource(i);

		if (i >= numSrcs) {
			// Just paint the background and continue
			paintAreaWithColor(sourceX, sourceY, scenesCX, scenesCY,
					   outerColor);
			paintAreaWithColor(siX, siY, siCX, siCY,
					   backgroundColor);
			continue;
		}

		OBSSource src = OBSGetStrongRef(multiviewScenes[i]);

		// We have a source. Now chose the proper highlight color
		uint32_t colorVal = outerColor;
		if (src == programSrc)
			colorVal = programColor;
		else if (src == previewSrc)
			colorVal = studioMode ? previewColor : programColor;

		// Paint the background
		paintAreaWithColor(sourceX, sourceY, scenesCX, scenesCY,
				   colorVal);
		paintAreaWithColor(siX, siY, siCX, siCY, backgroundColor);

		/* ----------- */

		// Render the source
		gs_matrix_push();
		gs_matrix_translate3f(siX, siY, 0.0f);
		gs_matrix_scale3f(siScaleX, siScaleY, 1.0f);
		setRegion(siX, siY, siCX, siCY);
		obs_source_video_render(src);
		endRegion();
		gs_matrix_pop();

		/* ----------- */

		// Render the label
		if (!drawLabel)
			continue;

		obs_source *label = multiviewLabels[i + 2];
		if (!label)
			continue;

		offset = labelOffset(multiviewLayout, label, scenesCX);

		gs_matrix_push();
		gs_matrix_translate3f(sourceX + offset,
				      (scenesCY * 0.85f) + sourceY, 0.0f);
		gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
		drawBox(obs_source_get_width(label),
			obs_source_get_height(label) + int(sourceY * 0.015f),
			labelColor);
		obs_source_video_render(label);
		gs_matrix_pop();
	}

	if (multiviewLayout == MultiviewLayout::SCENES_ONLY_4_SCENES ||
	    multiviewLayout == MultiviewLayout::SCENES_ONLY_9_SCENES ||
	    multiviewLayout == MultiviewLayout::SCENES_ONLY_16_SCENES ||
	    multiviewLayout == MultiviewLayout::SCENES_ONLY_25_SCENES) {
		endRegion();
		return;
	}

	/* ----------------------------- */
	/* draw preview                  */

	obs_source_t *previewLabel = multiviewLabels[0];
	offset = labelOffset(multiviewLayout, previewLabel, pvwprgCX);
	calcPreviewProgram(false);

	// Paint the background
	paintAreaWithColor(sourceX, sourceY, ppiCX, ppiCY, backgroundColor);

	// Scale and Draw the preview
	gs_matrix_push();
	gs_matrix_translate3f(sourceX, sourceY, 0.0f);
	gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
	setRegion(sourceX, sourceY, ppiCX, ppiCY);
	if (studioMode)
		obs_source_video_render(previewSrc);
	else
		obs_render_main_texture();

	if (drawSafeArea) {
		RenderSafeAreas(actionSafeMargin, targetCX, targetCY);
		RenderSafeAreas(graphicsSafeMargin, targetCX, targetCY);
		RenderSafeAreas(fourByThreeSafeMargin, targetCX, targetCY);
		RenderSafeAreas(leftLine, targetCX, targetCY);
		RenderSafeAreas(topLine, targetCX, targetCY);
		RenderSafeAreas(rightLine, targetCX, targetCY);
	}

	endRegion();
	gs_matrix_pop();

	/* ----------- */

	// Draw the Label
	if (drawLabel) {
		gs_matrix_push();
		gs_matrix_translate3f(labelX, labelY, 0.0f);
		gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
		drawBox(obs_source_get_width(previewLabel),
			obs_source_get_height(previewLabel) +
				int(pvwprgCX * 0.015f),
			labelColor);
		obs_source_video_render(previewLabel);
		gs_matrix_pop();
	}

	/* ----------------------------- */
	/* draw program                  */

	obs_source_t *programLabel = multiviewLabels[1];
	offset = labelOffset(multiviewLayout, programLabel, pvwprgCX);
	calcPreviewProgram(true);

	paintAreaWithColor(sourceX, sourceY, ppiCX, ppiCY, backgroundColor);

	// Scale and Draw the program
	gs_matrix_push();
	gs_matrix_translate3f(sourceX, sourceY, 0.0f);
	gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
	setRegion(sourceX, sourceY, ppiCX, ppiCY);
	obs_render_main_texture();
	endRegion();
	gs_matrix_pop();

	/* ----------- */

	// Draw the Label
	if (drawLabel) {
		gs_matrix_push();
		gs_matrix_translate3f(labelX, labelY, 0.0f);
		gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
		drawBox(obs_source_get_width(programLabel),
			obs_source_get_height(programLabel) +
				int(pvwprgCX * 0.015f),
			labelColor);
		obs_source_video_render(programLabel);
		gs_matrix_pop();
	}

	// Draw audioMeter on Program
	if (drawAudioMeter) {
		RenderAudioMeter();
	}

	// Region for future usage with additional info.
	if (multiviewLayout == MultiviewLayout::HORIZONTAL_TOP_24_SCENES) {
		// Just paint the background for now
		paintAreaWithColor(thickness, thickness, siCX,
				   siCY * 2 + thicknessx2, backgroundColor);
		paintAreaWithColor(thickness + 2.5 * (thicknessx2 + ppiCX),
				   thickness, siCX, siCY * 2 + thicknessx2,
				   backgroundColor);
	}

	endRegion();
}

OBSSource Multiview::GetSourceByPosition(int x, int y)
{
	int pos = -1;
	QWidget *rec = QApplication::activeWindow();
	if (!rec)
		return nullptr;
	int cx = rec->width();
	int cy = rec->height();
	int minX = 0;
	int minY = 0;
	int maxX = cx;
	int maxY = cy;

	switch (multiviewLayout) {
	case MultiviewLayout::HORIZONTAL_TOP_18_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
		}
		minY = cy / 2;

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 6);
		pos += ((y - minY) / ((maxY - minY) / 3)) * 6;

		break;
	case MultiviewLayout::HORIZONTAL_TOP_24_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
			minY = cy / 3;
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
			minY = (cy / 2) - (validY / 6);
		}

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 6);
		pos += ((y - minY) / ((maxY - minY) / 4)) * 6;

		break;
	case MultiviewLayout::VERTICAL_LEFT_8_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			minY = (cy / 2) - (validY / 2);
			maxY = (cy / 2) + (validY / 2);
		}

		minX = cx / 2;

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = 2 * ((y - minY) / ((maxY - minY) / 4));
		if (x > minX + ((maxX - minX) / 2))
			pos++;
		break;
	case MultiviewLayout::VERTICAL_RIGHT_8_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
		} else {
			int validY = cx / ratio;
			minY = (cy / 2) - (validY / 2);
			maxY = (cy / 2) + (validY / 2);
		}

		maxX = (cx / 2);

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = 2 * ((y - minY) / ((maxY - minY) / 4));
		if (x > minX + ((maxX - minX) / 2))
			pos++;
		break;
	case MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			minY = (cy / 2) - (validY / 2);
		}

		maxY = (cy / 2);

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 4);
		if (y > minY + ((maxY - minY) / 2))
			pos += 4;
		break;
	case MultiviewLayout::SCENES_ONLY_4_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
			minY = (cy / 2) - (validY / 2);
		}

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 2);
		pos += ((y - minY) / ((maxY - minY) / 2)) * 2;

		break;
	case MultiviewLayout::SCENES_ONLY_9_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
			minY = (cy / 2) - (validY / 2);
		}

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 3);
		pos += ((y - minY) / ((maxY - minY) / 3)) * 3;

		break;
	case MultiviewLayout::SCENES_ONLY_16_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
			minY = (cy / 2) - (validY / 2);
		}

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 4);
		pos += ((y - minY) / ((maxY - minY) / 4)) * 4;

		break;
	case MultiviewLayout::SCENES_ONLY_25_SCENES:
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
			minY = (cy / 2) - (validY / 2);
		}

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 5);
		pos += ((y - minY) / ((maxY - minY) / 5)) * 5;

		break;
	default: // MultiviewLayout::HORIZONTAL_TOP_8_SCENES
		if (float(cx) / float(cy) > ratio) {
			int validX = cy * ratio;
			minX = (cx / 2) - (validX / 2);
			maxX = (cx / 2) + (validX / 2);
		} else {
			int validY = cx / ratio;
			maxY = (cy / 2) + (validY / 2);
		}

		minY = (cy / 2);

		if (x < minX || x > maxX || y < minY || y > maxY)
			break;

		pos = (x - minX) / ((maxX - minX) / 4);
		if (y > minY + ((maxY - minY) / 2))
			pos += 4;
	}

	if (pos < 0 || pos >= (int)numSrcs)
		return nullptr;
	return OBSGetStrongRef(multiviewScenes[pos]);
}

//TOD: Refactor AudioMeterDrawing into new class
void Multiview::InitAudioMeter()
{
	//Gether audioSources
	uint32_t channelId = 1;
	minimumLevel = -60.0f;
	std::vector<std::string> channels = {"desktop1", "desktop2", "mic1",
					     "mic2",     "mic3",     "mic4"};
	for (auto &channel : channels) {
		const char *name;
		OBSSourceAutoRelease input = obs_get_output_source(channelId);
		if (input) {
			audioSource.emplace_back(OBSGetWeakRef(input));
			name = obs_source_get_name(input);
		}
		channelId++;
	}

	obs_volmeter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_add_callback(obs_volmeter, OBSVolumeLevelChanged, this);

	OBSSource src = OBSGetStrongRef(audioSource[0]);
	obs_volmeter_attach_source(obs_volmeter, src);
}

#define VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS 10
#define HORIZONTAL_PADDING_OF_VOLUME_METER 50
#define NUMBER_OF_VOLUME_METER_RECTENGELS 36
void Multiview::RenderAudioMeter()
{
	//calcPreviewProgram(true);

	auto drawBox = [&](float cx, float cy, uint32_t colorVal) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color =
			gs_effect_get_param_by_name(solid, "color");

		gs_effect_set_color(color, colorVal);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, (uint32_t)cx, (uint32_t)cy);
	};
	auto paintAreaWithColor = [&](float tx, float ty, float cx, float cy,
				      uint32_t color) {
		gs_matrix_push();
		gs_matrix_translate3f(tx, ty, 0.0f);
		drawBox(cx, cy, color);
		gs_matrix_pop();
	};

	int drawableChannels = 0;

	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		if (!isfinite(currentMagnitude[channelNr]))
			continue;
		drawableChannels++;
	}

	float scale = NUMBER_OF_VOLUME_METER_RECTENGELS / minimumLevel;
	float unusableSpace = (VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS *
			       (NUMBER_OF_VOLUME_METER_RECTENGELS + 5));
	float sizeOfRectengles = ppiCY / ((ppiCY - unusableSpace) /
					  (NUMBER_OF_VOLUME_METER_RECTENGELS));
	float xCoordinate = sourceX + 50;
	float yCoordinate = 0;
	size_t textvectorSize = multiviewLabels.size();
	obs_source_t *decibelLabel = multiviewLabels[textvectorSize - 1];
	float labelWidth = obs_source_get_width(decibelLabel);
	//Draw Background
	//---------------
	gs_matrix_push();
	paintAreaWithColor(
		xCoordinate - 20,
		sourceY + VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS,
		(ppiCX / sizeOfRectengles) * drawableChannels +
			HORIZONTAL_PADDING_OF_VOLUME_METER *
			(drawableChannels - 1) + 40,
		ppiCY - VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS * 2,
		labelColor);
	gs_matrix_pop();

	//Draw VU meter
	//-------------
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		if (!isfinite(currentMagnitude[channelNr]))
			continue;

		float offsetY = VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS * 2;
		offsetY += VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS;
		offsetY += ppiCY / sizeOfRectengles;
		yCoordinate = sourceY + ppiCY - offsetY;

		float lastValue = currentMagnitude[channelNr];

		int drawBars = NUMBER_OF_VOLUME_METER_RECTENGELS -
			       convertToInt(lastValue * scale);
		if (drawBars < 0)
			drawBars = 0;

		int soundAfter = minimumLevel;

		int indexOfScale = 1;

		if (isfinite(currentMagnitude[channelNr + 1])) {
			DrawScale(indexOfScale,
				  xCoordinate + ppiCX / sizeOfRectengles + 5,
				  yCoordinate);
			soundAfter += 5;
			indexOfScale++;
		}
		for (int i = 0; i < NUMBER_OF_VOLUME_METER_RECTENGELS; i++) {

			float sound = minimumLevel - (i + 1) / scale;
			uint32_t selectedColor = backgroundNominalColor;

			if (i < drawBars) {
				if (sound > -6) {
					selectedColor = foregroundErrorColor;
				} else if (sound > -20) {
					selectedColor = foregroundWarningColor;
				} else {
					selectedColor = foregroundNominalColor;
				}
				paintAreaWithColor(xCoordinate, yCoordinate,
						   ppiCX / sizeOfRectengles,
						   ppiCY / sizeOfRectengles,
						   selectedColor);
			} else {
				if (sound > -6) {
					selectedColor = backgroundErrorColor;
				} else if (sound > -20) {
					selectedColor = backgroundWarningColor;
				} else {
					selectedColor = backgroundNominalColor;
				}
				paintAreaWithColor(xCoordinate, yCoordinate,
						   ppiCX / sizeOfRectengles,
						   ppiCY / sizeOfRectengles,
						   selectedColor);
			}

			//Draw Scale by 5dB -s
			//-----------

			if (isfinite(currentMagnitude[channelNr + 1])) {
				if (round(sound) >= soundAfter) {
					DrawScale(
						indexOfScale,
						xCoordinate +
							ppiCX / sizeOfRectengles +
							5,
						yCoordinate);
					soundAfter += 5;
					indexOfScale++;
				}
			}
			offsetY += VERTICAL_PADDING_OF_VOLUME_METER_RECTENGELS;
			offsetY += ppiCY / sizeOfRectengles;
			yCoordinate = sourceY + ppiCY - offsetY;
		}
		xCoordinate += HORIZONTAL_PADDING_OF_VOLUME_METER +
			       ppiCX / sizeOfRectengles;
	}
}

void Multiview::DrawScale(int indexFromLast, float xCoordinate,
			  float yCoordinate)
{
	obs_source_t *decibelLabel =
		multiviewLabels[multiviewLabels.size() - indexFromLast];
	gs_matrix_push();
	gs_matrix_translate3f(xCoordinate, yCoordinate, 0.0f);
	//gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
	obs_source_video_render(decibelLabel);
	gs_matrix_pop();
}

void Multiview::OBSVolumeLevelChanged(void *data,
				      const float magnitude[MAX_AUDIO_CHANNELS],
				      const float peak[MAX_AUDIO_CHANNELS],
				      const float inputPeak[MAX_AUDIO_CHANNELS])
{
	Multiview *volControl = static_cast<Multiview *>(data);

	volControl->setLevels(magnitude, peak, inputPeak);
}

void Multiview::setLevels(const float magnitude[MAX_AUDIO_CHANNELS],
			  const float peak[MAX_AUDIO_CHANNELS],
			  const float inputPeak[MAX_AUDIO_CHANNELS])
{
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		currentMagnitude[channelNr] = magnitude[channelNr];
	}
}

inline int Multiview::convertToInt(float number)
{
	constexpr int min = std::numeric_limits<int>::min();
	constexpr int max = std::numeric_limits<int>::max();

	// NOTE: Conversion from 'const int' to 'float' changes max value from 2147483647 to 2147483648
	if (number >= (float)max)
		return max;
	else if (number < min)
		return min;
	else
		return int(number);
}
float Multiview::round(float var)
{
	// 37.66666 * 100 =3766.66
	// 3766.66 + .5 =3767.16    for rounding off value
	// then type cast to int so value is 3767
	// then divided by 100 so the value converted into 37.67
	float value = (int)(var * 100 + .5);
	return (float)value / 100;
}
