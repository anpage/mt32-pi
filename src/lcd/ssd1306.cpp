//
// ssd1306.cpp
//
// mt32-pi - A bare-metal Roland MT-32 emulator for Raspberry Pi
// Copyright (C) 2020  Dale Whinham <daleyo@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <algorithm>
#include <type_traits>

#include "lcd/font6x8.h"
#include "lcd/ssd1306.h"

// Compile-time (constexpr) font conversion functions.
// The SSD1306 stores pixel data in columns, but our source font data is stored as rows.
// These templated functions generate column-wise versions of our font at compile-time.
namespace
{
	using CharData = u8[8];

	// Iterate through each row of the character data and collect bits for the nth column
	static constexpr u8 SingleColumn(const CharData& CharData, u8 nColumn)
	{
		u8 bit = 5 - nColumn;
		u8 column = 0;

		for (u8 i = 0; i < 8; ++i)
			column |= (CharData[i] >> bit & 1) << i;

		return column;
	}

	// Double the height of the character by duplicating column bits into a 16-bit value
	static constexpr u16 DoubleColumn(const CharData& CharData, u8 nColumn)
	{
		u8 singleColumn = SingleColumn(CharData, nColumn);
		u16 column = 0;

		for (u8 i = 0; i < 8; ++i)
		{
			bool bit = singleColumn >> i & 1;
			column |= bit << i * 2 | bit << (i * 2 + 1);
		}

		return column;
	}

	// Templated array-like structure with precomputed font data
	template<size_t N, class F>
	class Font
	{
	public:
		// Result type of conversion function determines array type
		using Column = typename std::result_of<F& (const CharData&, u8)>::type;
		using ColumnData = Column[6];

		constexpr Font(const CharData(&CharData)[N], F Function) : mCharData{ 0 }
		{
			for (size_t i = 0; i < N; ++i)
				for (u8 j = 0; j < 6; ++j)
					mCharData[i][j] = Function(CharData[i], j);
		}

		const ColumnData& operator[](size_t nIndex) const { return mCharData[nIndex]; }

	private:
		ColumnData mCharData[N];
	};

	// Return number of elements in an array
	template<class T, size_t N>
	constexpr size_t ArraySize(const T(&)[N]) { return N; }
}

// Single and double-height versions of the font
constexpr auto FontSingle = Font<ArraySize(Font6x8), decltype(SingleColumn)>(Font6x8, SingleColumn);
constexpr auto FontDouble = Font<ArraySize(Font6x8), decltype(DoubleColumn)>(Font6x8, DoubleColumn);

const u8 CSSD1306::InitSequence[] =
{
	0xAE,       /* Screen off */
	0x81,       /* Set contrast */
		0x7F,   /* 00-FF, default to half */
	
	0xA6,       /* Normal display */

	0x20,       /* Set memory addressing mode */
		0x0,    /* 00 = horizontal */
	0x21,       /* Set column start and end address */
		0x00,
		0x7F,
	0x22,       /* Set page address range */
		0x00,
		0x03,
	
	0xA1,       /* Set segment remap */
	0xA8,       /* Set multiplex ratio */
		0x1F,   /* Screen height - 1 (31) */
	
	0xC8,       /* Set COM output scan direction */
	0xD3,       /* Set display offset */
		0x00,   /* None */
	0xDA,       /* Set com pins hardware configuration */
		0x02,   /* Alternate COM config and disable COM left/right */

	0xD5,       /* Set display oscillator */
		0x80,   /* Default value */
	0xD9,       /* Set precharge period */
		0x22,   /* Default value */
	0xDB,       /* Set VCOMH deselected level */
		0x20,   /* Default */

	0x8D,       /* Set charge pump */
	0x14,       /* VCC generated by internal DC/DC circuit */

	0xA4,       /* Resume to RAM content display */
	0xAF        /* Set display on */
};

CSSD1306::CSSD1306(CI2CMaster *pI2CMaster, u8 nAddress, u8 nHeight)
	: CMT32LCD(),
	  mI2CMaster(pI2CMaster),
	  mAddress(nAddress),
	  mHeight(nHeight),

	  mFramebuffer{0x40}
{
	assert(nHeight == 32 || nHeight == 64);
}

bool CSSD1306::Initialize()
{
	assert(mI2CMaster != nullptr);

	if (!(mHeight == 32 || mHeight == 64))
		return false;

	u8 buffer[] = {0x80, 0};
	for (auto byte : InitSequence)
	{
		buffer[1] = byte;
		mI2CMaster->Write(mAddress, buffer, 2);
	}

	return true;
}

void CSSD1306::WriteFramebuffer() const
{
	// Copy entire framebuffer
	mI2CMaster->Write(mAddress, mFramebuffer, mHeight * 16 + 1);
}

void CSSD1306::SetPixel(u8 nX, u8 nY)
{
	// Ensure range is within 0-127 for x, 0-63 for y
	nX &= 0x7f;
	nY &= 0x3f;

	// The framebuffer starts with the 0x40 byte so that we can write out the entire
	// buffer to the I2C device in one shot, so offset by 1
	mFramebuffer[((nY & 0xf8) << 4) + nX + 1] |= 1 << (nY & 7);
}

void CSSD1306::ClearPixel(u8 nX, u8 nY)
{
	// Ensure range is within 0-127 for x, 0-63 for y
	nX &= 0x7f;
	nY &= 0x3f;

	mFramebuffer[((nY & 0xf8) << 4) + nX + 1] &= ~(1 << (nY & 7));
}

void CSSD1306::DrawChar(char chChar, u8 nCursorX, u8 nCursorY, bool bInverted, bool bDoubleWidth)
{
	size_t rowOffset = nCursorY * 128 * 2;
	size_t columnOffset = nCursorX * (bDoubleWidth ? 12 : 6) + 4;

	// FIXME: Won't be needed when the full font is implemented in font6x8.h
	if (chChar == '\xFF')
		chChar = '\x80';

	for (u8 i = 0; i < 6; ++i)
	{
		u16 fontColumn = FontDouble[static_cast<u8>(chChar - ' ')][i];

		// Don't invert the leftmost column or last two rows
		if (i > 0 && bInverted)
			fontColumn ^= 0x3FFF;

		// Upper half of font
		size_t offset = rowOffset + columnOffset + (bDoubleWidth ? i * 2 : i);

		mFramebuffer[offset] = fontColumn & 0xFF;
		mFramebuffer[offset + 128] = (fontColumn >> 8) & 0xFF;
		if (bDoubleWidth)
		{
			mFramebuffer[offset + 1] = mFramebuffer[offset];
			mFramebuffer[offset + 128 + 1] = mFramebuffer[offset + 128];
		}
	}
}

void CSSD1306::DrawPartLevels(bool bDrawPeaks)
{
	for (u8 i = 0; i < 9; ++i)
	{
		// Bar graphs
		u8 topVal, bottomVal;
		if (mPartLevels[i] > 8)
		{
			topVal = 0xFF << (8 - (mPartLevels[i] - 8));
			bottomVal = 0xFF;
		}
		else
		{
			topVal = 0x00;
			bottomVal = 0xFF << (8 - (mPartLevels[i]));
		}

		// Peak meters
		if (bDrawPeaks)
		{
			if (mPeakLevels[i] > 8)
				topVal |= 1 << (8 - (mPeakLevels[i] - 8));
			else
				bottomVal |= 1 << (8 - (mPeakLevels[i]));

			for (u8 j = 0; j < 12; ++j)
			{
				mFramebuffer[256 + i * 14 + j + 3] = topVal;	
				mFramebuffer[256 + i * 14 + j + 128 + 3] = bottomVal;
			}
		}
	}
}

void CSSD1306::Print(const char* pText, u8 nCursorX, u8 nCursorY, bool bClearLine, bool bImmediate)
{
	while (*pText && nCursorX < 20)
	{
		DrawChar(*pText++, nCursorX, nCursorY);
		++nCursorX;
	}

	if (bClearLine)
	{
		while (nCursorX < 20)
			DrawChar(' ', nCursorX++, nCursorY);
	}

	if (bImmediate)
		WriteFramebuffer();
}

void CSSD1306::Clear()
{
	std::fill(mFramebuffer + 1, mFramebuffer + mHeight * 16 + 1, 0);
	WriteFramebuffer();
}

void CSSD1306::Update(const CMT32SynthBase& Synth)
{
	CMT32LCD::Update(Synth);

	UpdatePartLevels(Synth);
	UpdatePeakLevels();

	Print(mTextBuffer, 0, 0, true);
	DrawPartLevels();
	WriteFramebuffer();
}
