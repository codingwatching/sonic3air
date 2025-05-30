/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2025 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"

#ifdef RMX_WITH_OPENGL_SUPPORT

#include "oxygen/rendering/opengl/shaders/PostFXBlurShader.h"
#include "oxygen/helper/FileHelper.h"


void PostFXBlurShader::initialize()
{
	if (FileHelper::loadShader(mShader, L"data/shader/postfx_blur.shader", "Standard"))
	{
		bindShader();

		mLocMainTexture = mShader.getUniformLocation("MainTexture");
		mLocTexelOffset = mShader.getUniformLocation("TexelOffset");
		mLocKernel      = mShader.getUniformLocation("Kernel");
	}
}

void PostFXBlurShader::draw(GLuint textureHandle, const Vec2f& texelOffset, const Vec4f& kernel)
{
	bindShader();

	// Bind textures
	mShader.setTexture(mLocMainTexture, textureHandle, GL_TEXTURE_2D);

	// Update uniforms
	mShader.setParam(mLocTexelOffset, texelOffset);
	mShader.setParam(mLocKernel, kernel);

	// Draw
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

#endif
