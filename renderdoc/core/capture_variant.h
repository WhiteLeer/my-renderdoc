/******************************************************************************
 * Capture files produced by the custom SR RenderDoc build are intentionally
 * separated from captures produced by other custom builds.
 ******************************************************************************/

#pragma once

// Set these three values per game-specific RenderDoc build. The replay code
// remains generic and validates both the extension and the embedded marker.
#define RENDERDOC_CAPTURE_FILE_EXTENSION "srrdc"
#define RENDERDOC_CAPTURE_FILE_SUFFIX ".srrdc"
#define RENDERDOC_CAPTURE_VARIANT "SR"
#define RENDERDOC_CAPTURE_VARIANT_SECTION "RenderDocCaptureVariant"
