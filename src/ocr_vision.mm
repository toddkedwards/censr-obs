/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* macOS Vision (VNRecognizeTextRequest) implementation of ICensrOcr. The Mac
 * standalone app uses the same framework, so this backend reuses the exact
 * detection stack the pipeline was built on. No apartment/threading setup, unlike
 * the Windows.Media.Ocr backend. */

#include <util/base.h>

#include "ocr_engine.h"
#include "plugin-support.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Vision/Vision.h>

#include <algorithm>

namespace censr {

namespace {

/* Vision boxes are normalized (0..1) with a BOTTOM-left origin; the pipeline
 * wants pixels with a TOP-left origin. */
RectF to_pixels(CGRect bb, int w, int h)
{
	RectF r;
	r.x = (float)(bb.origin.x * w);
	r.w = (float)(bb.size.width * w);
	r.h = (float)(bb.size.height * h);
	r.y = (float)((1.0 - bb.origin.y - bb.size.height) * h);
	return r;
}

class VisionOcr : public ICensrOcr {
public:
	std::vector<OcrLine> recognize(const uint8_t *bgra, int w, int h, int stride) override
	{
		std::vector<OcrLine> out;
		if (!bgra || w <= 0 || h <= 0)
			return out;

		@autoreleasepool {
			/* Wrap the caller's BGRA8 buffer as a CGImage (no copy; the
			 * request runs synchronously so the buffer stays valid). */
			CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
			/* Cast to ints before OR: the SDK deprecates bitwise ops between
			 * the two Core Graphics enum types (and -Werror promotes it). */
			CGBitmapInfo info = (CGBitmapInfo)((uint32_t)kCGImageAlphaNoneSkipFirst |
							   (uint32_t)kCGBitmapByteOrder32Little);
			CGDataProviderRef provider = CGDataProviderCreateWithData(
				nullptr, bgra, (size_t)stride * h, nullptr);
			CGImageRef image = CGImageCreate((size_t)w, (size_t)h, 8, 32,
							 (size_t)stride, cs, info, provider,
							 nullptr, false, kCGRenderingIntentDefault);
			CGDataProviderRelease(provider);
			CGColorSpaceRelease(cs);
			if (!image)
				return out;

			VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
			req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
			/* Secrets are random tokens, not prose — language correction
			 * would "fix" them into the wrong thing. */
			req.usesLanguageCorrection = NO;
			req.recognitionLanguages = @[ @"en-US" ];

			VNImageRequestHandler *handler =
				[[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
			NSError *err = nil;
			[handler performRequests:@[ req ] error:&err];
			CGImageRelease(image);
			if (err) {
				obs_log(LOG_WARNING, "Censr Vision OCR error: %s",
					err.localizedDescription.UTF8String);
				return out;
			}

			for (VNRecognizedTextObservation *obs in req.results) {
				VNRecognizedText *cand = [[obs topCandidates:1] firstObject];
				if (!cand)
					continue;
				NSString *text = cand.string;
				if (text.length == 0)
					continue;

				OcrLine ol;
				ol.text = text.UTF8String;
				ol.box = to_pixels(obs.boundingBox, w, h);

				/* Best-effort word boxes: Vision can give a tight box for
				 * any substring range. Cover the whole line via ol.box if a
				 * range lookup fails. */
				NSScanner *scanner = [NSScanner scannerWithString:text];
				scanner.charactersToBeSkipped = nil;
				NSCharacterSet *ws = [NSCharacterSet whitespaceAndNewlineCharacterSet];
				while (!scanner.atEnd) {
					NSString *token = nil;
					[scanner scanCharactersFromSet:ws intoString:NULL];
					if ([scanner scanUpToCharactersFromSet:ws intoString:&token] &&
					    token.length > 0) {
						NSRange r = [text rangeOfString:token];
						OcrWord ow;
						ow.text = token.UTF8String;
						VNRectangleObservation *ro =
							[cand boundingBoxForRange:r error:nil];
						ow.box = ro ? to_pixels(ro.boundingBox, w, h) : ol.box;
						ol.words.push_back(std::move(ow));
					}
				}
				out.push_back(std::move(ol));
			}
		}
		return out;
	}
};

} // namespace

std::unique_ptr<ICensrOcr> create_vision_ocr()
{
	obs_log(LOG_INFO, "Censr: macOS Vision OCR engine ready");
	return std::make_unique<VisionOcr>();
}

} // namespace censr
