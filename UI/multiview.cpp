#include "multiview.hpp"
#include "window-basic-main.hpp"
#include "obs-app.hpp"
#include "platform.hpp"
#include "display-helpers.hpp"
#include "obs-audio-controls.h"
#include "obs.h"
#include "obs-source.h"
#include "util/threading.h"

#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#define NUMBER_OF_VOLUME_METER_RECTENGELS 48
#define DRAW_SCALE_NUMBERS_INCREMENT 5
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

	obs_remove_raw_audio_callback(selectedTrackIndex,
				      OBSOutputVolumeLevelChanged, this);

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
		       bool drawSafeArea, bool drawAudioMeter,
		       int selectedNewAudio)
{
	this->multiviewLayout = multiviewLayout;
	this->drawLabel = drawLabel;
	this->drawSafeArea = drawSafeArea;
	this->drawAudioMeter = drawAudioMeter;
	if (this->selectedAudio != selectedNewAudio) {

		obs_remove_raw_audio_callback(
			selectedTrackIndex, OBSOutputVolumeLevelChanged, this);
		this->selectedAudio = selectedNewAudio;
	}

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
		     deciBells -= DRAW_SCALE_NUMBERS_INCREMENT) {
			char integer_string[4];

			sprintf(integer_string, "%d", deciBells);
			multiviewLabels.emplace_back(
				CreateLabel(integer_string, h / 4));
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

//TODO: Refactor AudioMeterDrawing into new class
void Multiview::InitAudioMeter()
{
	minimumLevel = -60.0f;
	//Gether audioSources
	/*uint32_t channelId = 1;
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
	obs_volmeter_attach_source(obs_volmeter, src);*/
}
void Multiview::ConnectAudioOutput()
{
	if (selectedAudio != pow(2, selectedTrackIndex)) {
		if (selectedAudio & (1 << 0)) {
			selectedTrackIndex = 0;
		} else if (selectedAudio & (1 << 1)) {
			selectedTrackIndex = 1;
		} else if (selectedAudio & (1 << 2)) {
			selectedTrackIndex = 2;
		} else if (selectedAudio & (1 << 3)) {
			selectedTrackIndex = 3;
		} else if (selectedAudio & (1 << 4)) {
			selectedTrackIndex = 4;
		} else if (selectedAudio & (1 << 5)) {
			selectedTrackIndex = 5;
		}
		struct audio_convert_info *arg2 =
			(struct audio_convert_info *)0;

		obs_add_raw_audio_callback(
			selectedTrackIndex,
			(struct audio_convert_info const *)arg2,
			OBSOutputVolumeLevelChanged, this);
	}
}

void Multiview::RenderAudioMeter()
{
	//calcPreviewProgram(true);
	ConnectAudioOutput();

	auto drawBox = [&](float cx, float cy, uint32_t colorVal) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color =
			gs_effect_get_param_by_name(solid, "color");

		gs_effect_set_color(color, colorVal);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, (uint32_t)cx, (uint32_t)cy);
	};

	struct rectangleForDraw {
		float XPoint;
		float YPoint;
		float Width;
		float Height;
	};

	auto paintAreaWithColor =
		[&](rectangleForDraw
			    rect, /* float tx, float ty, float cx, float cy,*/
		    uint32_t color) {
			gs_matrix_push();
			gs_matrix_translate3f(rect.XPoint, rect.YPoint, 0.0f);
			drawBox(rect.Width, rect.Height, color);
			gs_matrix_pop();
		};

	int drawableChannels = 0;
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		if (!isfinite(currentMagnitude[channelNr]))
			continue;
		drawableChannels++;
	}

	//fallback to stereo if no channel found
	if (drawableChannels == 0) {
		drawableChannels = 2;
	}

	float scale = NUMBER_OF_VOLUME_METER_RECTENGELS / minimumLevel;
	float sizeOfRectengles = NUMBER_OF_VOLUME_METER_RECTENGELS;

	//AddSpaceing between rectangles
	sizeOfRectengles += (NUMBER_OF_VOLUME_METER_RECTENGELS - 1) / 2;
	//Add padding
	sizeOfRectengles += 5;
	float yRectSize = ppiCY / sizeOfRectengles;
	float xRectSize = ppiCX / sizeOfRectengles;
	float xCoordinate = sourceX + xRectSize;
	float yCoordinate = sourceY + yRectSize;

	//Select the longest label in the scale
	//--------------------------------------
	int numberOfScaleLabel =
		(minimumLevel * -1 / DRAW_SCALE_NUMBERS_INCREMENT);
	size_t textvectorSize = multiviewLabels.size();
	float labelWidth = 0;
	float labelHeight = 0;
	for (int j = numberOfScaleLabel; j >= 0; j--) {
		obs_source_t *decibelLabel =
			multiviewLabels[textvectorSize - j - 1];
		if (labelWidth <
		    obs_source_get_width(decibelLabel) * ppiScaleX) {
			labelWidth =
				obs_source_get_width(decibelLabel) * ppiScaleX;
			labelHeight =
				obs_source_get_height(decibelLabel) * ppiScaleY;
		}
	}

	//Draw Background
	//---------------
	rectangleForDraw backRect;
	backRect.XPoint = xCoordinate;
	backRect.YPoint = yCoordinate;
	backRect.Width = xRectSize * drawableChannels +
			 labelWidth * (drawableChannels - 1) + xRectSize;
	backRect.Height = ppiCY - yRectSize * 2;
	gs_matrix_push();
	paintAreaWithColor(backRect, labelColor);
	gs_matrix_pop();

	//Add padding
	xCoordinate += xRectSize / 2;

	//Draw Scale by DRAW_SCALE_NUMBERS_INCREMENT dB -s
	//-----------
	for (int i = 1; i <= drawableChannels; i++) {
		float lableX =
			xCoordinate + xRectSize * i + labelWidth * (i - 1);
		if (isfinite(currentMagnitude[i])) {
			float yOffset = yRectSize / 2;
			float usableY = backRect.Height - yRectSize * 3;
			float pixelIncrementOfScale =
				usableY / numberOfScaleLabel;
			for (int j = numberOfScaleLabel; j >= 0; j--) {
				float labelY = yCoordinate + yOffset;
				yOffset += pixelIncrementOfScale;
				obs_source_t *curentDecibelLabel =
					multiviewLabels[textvectorSize - j - 1];
				float currentLabelWidth =
					obs_source_get_width(
						curentDecibelLabel) *
					ppiScaleX;
				float xOffset =
					(labelWidth - currentLabelWidth) / 2;
				DrawScale(j, lableX + xOffset, labelY);
			}
		}
	}

	//Draw VU meter
	//-------------
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		if (!isfinite(currentMagnitude[channelNr]))
			continue;
		float offsetY = yRectSize * 3;
		yCoordinate = sourceY + ppiCY - offsetY;

		float lastValue = currentMagnitude[channelNr];

		int drawBars = NUMBER_OF_VOLUME_METER_RECTENGELS -
			       convertToInt(lastValue * scale);
		if (drawBars < 0)
			drawBars = 0;

		int soundAfter = minimumLevel;

		int indexOfScale = 1;

		for (int i = 0; i < NUMBER_OF_VOLUME_METER_RECTENGELS; i++) {

			float sound = minimumLevel - (i + 1) / scale;
			rectangleForDraw vuRectangle;
			vuRectangle.XPoint = xCoordinate;
			vuRectangle.YPoint = yCoordinate;
			vuRectangle.Width = xRectSize;
			vuRectangle.Height = yRectSize;
			if (i < drawBars) {
				if (sound > -6) {
					paintAreaWithColor(
						vuRectangle,
						foregroundErrorColor);
				} else if (sound > -20) {
					paintAreaWithColor(
						vuRectangle,
						foregroundWarningColor);
				} else {
					paintAreaWithColor(
						vuRectangle,
						foregroundNominalColor);
				}
			} else {
				if (sound > -6) {
					paintAreaWithColor(
						vuRectangle,
						backgroundErrorColor);
				} else if (sound > -20) {
					paintAreaWithColor(
						vuRectangle,
						backgroundWarningColor);
				} else {
					paintAreaWithColor(
						vuRectangle,
						backgroundNominalColor);
				}
			}
			offsetY += yRectSize / 2;
			offsetY += yRectSize;
			yCoordinate = sourceY + ppiCY - offsetY;
		}
		xCoordinate += xRectSize + labelWidth;
	}
}

void Multiview::DrawScale(int indexFromLast, float placeX, float placeY)
{
	obs_source_t *decibelLabel =
		multiviewLabels[multiviewLabels.size() - 1 - indexFromLast];
	gs_matrix_push();
	gs_matrix_translate3f(placeX, placeY, 0.0f);
	gs_matrix_scale3f(ppiScaleX, ppiScaleY, 1.0f);
	obs_source_video_render(decibelLabel);
	gs_matrix_pop();
}

void Multiview::OBSVolumeLevelChanged(void *data,
				      const float magnitude[MAX_AUDIO_CHANNELS],
				      const float peak[MAX_AUDIO_CHANNELS],
				      const float inputPeak[MAX_AUDIO_CHANNELS])
{
	Multiview *volControl = static_cast<Multiview *>(data);

	volControl->setLevels(magnitude);
}

//ListenForOutputLevelChange
void Multiview::OBSOutputVolumeLevelChanged(void *param, size_t mix_idx,
					    struct audio_data *data)
{
	Multiview *volControl = static_cast<Multiview *>(param);
	if (data) {

		int nr_channels = 0;
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			if (data->data[i])
				nr_channels++;
		}
		nr_channels = CLAMP(nr_channels, 0, MAX_AUDIO_CHANNELS);
		size_t nr_samples = data->frames;

		int channel_nr = 0;
		for (int plane_nr = 0; channel_nr < nr_channels; plane_nr++) {
			if (channel_nr < nr_channels) {
				float *samples = (float *)data->data[plane_nr];
				if (!samples) {
					continue;
				}

				float sum = 0.0;
				for (size_t i = 0; i < nr_samples; i++) {
					float sample = samples[i];
					sum += sample * sample;
				}
				volControl->currentMagnitude[channel_nr] =
					sqrtf(sum / nr_samples);
			} else {
				volControl->currentMagnitude[channel_nr] = 0.0f;
			}
			channel_nr++;
		}
		for (int channel_nr = 0; channel_nr < MAX_AUDIO_CHANNELS;
		     channel_nr++) {
			volControl->currentMagnitude[channel_nr] = obs_mul_to_db(
				volControl->currentMagnitude[channel_nr]);
		}
	}
}
//until now

void Multiview::setLevels(const float magnitude[MAX_AUDIO_CHANNELS])
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
