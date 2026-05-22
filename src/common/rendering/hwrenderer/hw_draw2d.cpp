/*
** hw_draw2d.cpp
** 2d drawer Renderer interface
**
**---------------------------------------------------------------------------
** Copyright 2018-2019 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "v_video.h"
#include "cmdlib.h"
#include "hwrenderer/data/buffers.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_renderstate.h"
#include "r_videoscale.h"
#include "v_draw.h"
#include "printf.h"

//===========================================================================
// 
// Draws the 2D stuff. This is the version for OpenGL 3 and later.
//
//===========================================================================

CVAR(Bool, gl_aalines, false, CVAR_ARCHIVE) 
CVAR(Bool, hw_2dmip, true, CVAR_ARCHIVE)

void Draw2D(F2DDrawer* drawer, FRenderState& state)
{
	const auto& mScreenViewport = screen->mScreenViewport;
	Draw2D(drawer, state, mScreenViewport.left, mScreenViewport.top, mScreenViewport.width, mScreenViewport.height);
}

void Draw2D(F2DDrawer* drawer, FRenderState& state, int x, int y, int width, int height)
{
	twoD.Clock();

	state.SetViewport(x, y, width, height);
	screen->mViewpoints->Set2D(state, drawer->GetWidth(), drawer->GetHeight());

	state.EnableStencil(false);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
	state.Clear(CT_Stencil);
	state.EnableDepthTest(false);
	state.EnableMultisampling(false);
	state.EnableLineSmooth(gl_aalines);

	bool cache_hw_2dmip = hw_2dmip && (!sysCallbacks.DisableAnisotropicFiltering || !sysCallbacks.DisableAnisotropicFiltering()); // cache cvar lookup so it's not done in a loop

	auto &vertices = drawer->mVertices;
	auto &indices = drawer->mIndices;
	auto &commands = drawer->mData;

	if (commands.Size() == 0)
	{
		twoD.Unclock();
		return;
	}

	if (drawer->mIsFirstPass)
	{
		for (auto &v : vertices)
		{
			// Change from BGRA to RGBA
			std::swap(v.color0.r, v.color0.b);
		}
	}
	F2DVertexBuffer vb;
	vb.UploadData(&vertices[0], vertices.Size(), &indices[0], indices.Size());
	state.SetVertexBuffer(&vb);
	state.EnableFog(false);

	for(auto &cmd : commands)
	{
		if (cmd.isSpecial != SpecialDrawCommand::NotSpecial)
		{
			if (cmd.isSpecial == SpecialDrawCommand::EnableStencil)
			{
				state.EnableStencil(cmd.stencilOn);
			}
			else if (cmd.isSpecial == SpecialDrawCommand::SetStencil)
			{
				state.SetStencil(cmd.stencilOffs, cmd.stencilOp, cmd.stencilFlags);
			}
			else if (cmd.isSpecial == SpecialDrawCommand::ClearStencil)
			{
				state.Clear(CT_Stencil);
			}
			continue;
		}

		state.SetRenderStyle(cmd.mRenderStyle);
		state.EnableBrightmap(!(cmd.mRenderStyle.Flags & STYLEF_ColorIsFixed));
		state.EnableFog(2);	// Special 2D mode 'fog'.
		state.SetScreenFade(cmd.mScreenFade);

		state.SetTextureMode(cmd.mDrawMode);

		int sciX, sciY, sciW, sciH;
		if (cmd.mFlags & F2DDrawer::DTF_Scissor)
		{
			// scissor test doesn't use the current viewport for the coordinates, so use real screen coordinates
			// Note that the origin here is the lower left corner!
			// Convert from drawer (zdoom screen) coordinates to the supplied viewport/window
			// coordinates. We can't use screen->ScreenToWindowX/Y here because that always
			// references the primary framebuffer's mScreenViewport/mGameScreenWidth. When this
			// function is invoked for an off-screen surface (e.g. the second-display lower HUD),
			// using the primary viewport produces wildly out-of-range scissor rectangles which
			// clip away most of the drawn content and causes severe flicker / disappearing
			// graphics whenever the source emits many DTA_Clip* commands (statusbar, counters).
			//
			// Callers frequently pass huge sentinel values (~INT_MAX/2) for "no clip" on a given
			// axis. Multiplying those by the destination dimension overflows int32 unless the
			// drawer/destination ratio is close to 1:1. Do the math in int64_t and clamp the
			// final rectangle to the destination viewport so that "no clip" sentinels collapse
			// to the full viewport instead of producing a wildly negative or empty box.
			const int64_t drawerW = drawer->GetWidth() > 0 ? drawer->GetWidth() : 1;
			const int64_t drawerH = drawer->GetHeight() > 0 ? drawer->GetHeight() : 1;
			const int64_t vpX = x;
			const int64_t vpY = y;
			const int64_t vpW = width;
			const int64_t vpH = height;
			auto toWinX = [&](int64_t sx) -> int64_t { return vpX + (sx * vpW) / drawerW; };
			auto toWinY = [&](int64_t sy) -> int64_t { return vpY + vpH - (sy * vpH) / drawerH; };
			// GZDoom / ZScript pass huge sentinel values (~INT_MAX or INT_MAX/2,
			// sometimes offset by a frame dimension) to mean "no clip on this axis".
			// We must detect these BEFORE scaling, otherwise after Y-flip they collapse
			// into a zero-height rectangle and silently cull whole HUD elements (the
			// root cause of the second-screen lower-HUD flicker: every other frame
			// the statusbar geometry overflows the drawer's virtual screen, triggers
			// DTF_Scissor, and the sentinel values produce an empty scissor).
			constexpr int64_t kScissorSentinel = (int64_t)1 << 28; // ~268M; real coords are << 1<<20
			auto isSentinel = [&](int v) -> bool {
				return (int64_t)v >  kScissorSentinel || (int64_t)v < -kScissorSentinel;
			};
			const bool noL = isSentinel(cmd.mScissor[0]);
			const bool noT = isSentinel(cmd.mScissor[1]);
			const bool noR = isSentinel(cmd.mScissor[2]);
			const bool noB = isSentinel(cmd.mScissor[3]);
			const int64_t vpRight = vpX + vpW;
			const int64_t vpTop   = vpY + vpH;
			int64_t left   = noL ? vpX     : toWinX((int64_t)cmd.mScissor[0]);
			int64_t right  = noR ? vpRight : toWinX((int64_t)cmd.mScissor[2]);
			// scissor uses drawer-bottom-up Y after Y-flip:
			// drawer top    -> high GL Y, drawer bottom -> low GL Y.
			int64_t bottom = noB ? vpY     : toWinY((int64_t)cmd.mScissor[3]);
			int64_t top    = noT ? vpTop   : toWinY((int64_t)cmd.mScissor[1]);
			// Clamp to viewport.
			if (left   < vpX)     left   = vpX;
			if (left   > vpRight) left   = vpRight;
			if (right  < vpX)     right  = vpX;
			if (right  > vpRight) right  = vpRight;
			if (bottom < vpY)     bottom = vpY;
			if (bottom > vpTop)   bottom = vpTop;
			if (top    < vpY)     top    = vpY;
			if (top    > vpTop)   top    = vpTop;
			sciX = (int)left;
			sciY = (int)bottom;
			sciW = (int)(right - left);
			sciH = (int)(top - bottom);
		}
		else
		{
			sciX = sciY = sciW = sciH = -1;
		}
		state.SetScissor(sciX, sciY, sciW, sciH);

		if (cmd.mSpecialColormap[0].a != 0)
		{
			state.SetTextureMode(TM_FIXEDCOLORMAP);
			state.SetObjectColor(cmd.mSpecialColormap[0]);
			state.SetAddColor(cmd.mSpecialColormap[1]);
		}
		state.SetFog(cmd.mColor1, 0);
		state.SetColor(1, 1, 1, 1, cmd.mDesaturate); 
		if (cmd.mFlags & F2DDrawer::DTF_Indexed) state.SetSoftLightLevel(cmd.mLightLevel);
		state.SetLightParms(0, 0);

		state.AlphaFunc(Alpha_Greater, 0.f);

		if (cmd.useTransform)
		{
			FLOATTYPE m[16] = {
				0.0, 0.0, 0.0, 0.0,
				0.0, 0.0, 0.0, 0.0,
				0.0, 0.0, 1.0, 0.0,
				0.0, 0.0, 0.0, 1.0
			};
			for (size_t i = 0; i < 2; i++)
			{
				for (size_t j = 0; j < 2; j++)
				{
					m[4 * j + i] = (FLOATTYPE) cmd.transform.Cells[i][j];
				}
			}
			for (size_t i = 0; i < 2; i++)
			{
				m[4 * 3 + i] = (FLOATTYPE) cmd.transform.Cells[i][2];
			}
			state.mModelMatrix.loadMatrix(m);
			state.EnableModelMatrix(true);
		}

		if (cmd.mTexture != nullptr && cmd.mTexture->isValid())
		{
			auto flags = cmd.mTexture->GetUseType() >= ETextureType::Special? UF_None : cmd.mTexture->GetUseType() == ETextureType::FontChar? UF_Font : UF_Texture;

			auto scaleflags = cmd.mFlags & F2DDrawer::DTF_Indexed ? CTF_Indexed : 0;
			state.SetMaterial(cmd.mTexture, flags, scaleflags, cmd.mFlags & F2DDrawer::DTF_Wrap ? CLAMP_NONE : (cache_hw_2dmip ? CLAMP_XY : CLAMP_XY_NOMIP), cmd.mTranslationId, -1);
			state.EnableTexture(true);

			// Canvas textures are stored upside down
			if (cmd.mTexture->isHardwareCanvas())
			{
				state.mTextureMatrix.loadIdentity();
				state.mTextureMatrix.scale(1.f, -1.f, 1.f);
				state.mTextureMatrix.translate(0.f, 1.f, 0.0f);
				state.EnableTextureMatrix(true);
			}
			if (cmd.mFlags & F2DDrawer::DTF_Burn)
			{
				state.SetEffect(EFF_BURN);
			}
		}
		else
		{
			state.EnableTexture(false);
		}

		if (cmd.shape2DBufInfo != nullptr)
		{
			state.SetVertexBuffer(&cmd.shape2DBufInfo->buffers[cmd.shape2DBufIndex]);
			state.DrawIndexed(DT_Triangles, 0, cmd.shape2DIndexCount);
			state.SetVertexBuffer(&vb);
			if (cmd.shape2DCommandCounter == cmd.shape2DBufInfo->lastCommand)
			{
				cmd.shape2DBufInfo->lastCommand = -1;
				if (cmd.shape2DBufInfo->bufIndex > 0)
				{
					cmd.shape2DBufInfo->needsVertexUpload = true;
					cmd.shape2DBufInfo->buffers.Clear();
					cmd.shape2DBufInfo->bufIndex = -1;
				}
			}
			cmd.shape2DBufInfo->uploadedOnce = false;
		}
		else
		{
			switch (cmd.mType)
			{
			default:
			case F2DDrawer::DrawTypeTriangles:
				state.DrawIndexed(DT_Triangles, cmd.mIndexIndex, cmd.mIndexCount);
				break;

			case F2DDrawer::DrawTypeLines:
				state.Draw(DT_Lines, cmd.mVertIndex, cmd.mVertCount);
				break;

			case F2DDrawer::DrawTypePoints:
				state.Draw(DT_Points, cmd.mVertIndex, cmd.mVertCount);
				break;

			}
		}
		state.SetObjectColor(0xffffffff);
		state.SetObjectColor2(0);
		state.SetAddColor(0);
		state.EnableTextureMatrix(false);
		state.EnableModelMatrix(false);
		state.SetEffect(EFF_NONE);

	}
	state.SetScissor(-1, -1, -1, -1);

	state.SetRenderStyle(STYLE_Translucent);
	state.SetVertexBuffer(screen->mVertexData);
	state.EnableStencil(false);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
	state.EnableTexture(true);
	state.EnableBrightmap(true);
	state.SetTextureMode(TM_NORMAL);
	state.EnableFog(false);
	state.SetScreenFade(1);
	state.SetSoftLightLevel(255);
	state.ResetColor();
	drawer->mIsFirstPass = false;
	twoD.Unclock();

}
