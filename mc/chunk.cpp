/*
 * Copyright 2012, 2013 Moritz Hilscher
 *
 * This file is part of mapcrafter.
 *
 * mapcrafter is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mapcrafter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mapcrafter.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mc/chunk.h"

#include "mc/nbt.h"

#include <iostream>
#include <cmath>
#include "MCblock.h"

namespace mapcrafter {
namespace mc {

Chunk::Chunk()
		: pos(42, 42), rotation(0) {
	clear();
}

Chunk::~Chunk() {
}

void Chunk::setRotation(int rotation) {
	this->rotation = rotation;
}

/**
 * Reads the chunk from (compressed) nbt data.
 */
bool Chunk::readNBT(const char* data, size_t len, nbt::CompressionType compression) {
	clear();

	nbt::NBTFile nbt;
	nbt.readNBT(data, len, compression);

	// find "level" tag
	nbt::TagCompound* level = nbt.findTag<nbt::TagCompound>("Level", nbt::TAG_COMPOUND);
	if (level == NULL) {
		std::cerr << "Warning: Corrupt chunk (No level tag)!" << std::endl;
		return false;
	}

	// then find x/z pos of the chunk
	nbt::TagInt* xpos = level->findTag<nbt::TagInt>("xPos", nbt::TAG_INT);
	nbt::TagInt* zpos = level->findTag<nbt::TagInt>("zPos", nbt::TAG_INT);
	if (xpos == NULL || zpos == NULL) {
		std::cerr << "Warning: Corrupt chunk (No x/z position found)!" << std::endl;
		return false;
	}
	pos = ChunkPos(xpos->payload, zpos->payload);
	if (rotation)
		pos.rotate(rotation);

	nbt::TagByteArray* tagBiomes = level->findTag<nbt::TagByteArray>("Biomes",
			nbt::TAG_BYTE_ARRAY);
	if (tagBiomes != NULL && tagBiomes->payload.size() == 256)
		std::copy(tagBiomes->payload.begin(), tagBiomes->payload.end(), biomes);
	else
		std::cerr << "Warning: Corrupt chunk at " << pos.x << ":" << pos.z
				<< " (No biome data found)!" << std::endl;

	// find sections list
	nbt::TagList* tagSections = level->findTag<nbt::TagList>("Sections", nbt::TAG_LIST);

	// I already saw (empty) chunks from the end with TagBytes instead of TagCompound
	// in this list, ignore them, they are empty
	if (tagSections == NULL
			|| (tagSections->payload.size() != 0 && tagSections->tag_type != nbt::TAG_COMPOUND)) {
		std::cerr << "Warning: Corrupt chunk at " << pos.x << ":" << pos.z
		        << " (No valid sections list found)!" << std::endl;
		return false;
	} else if (tagSections->payload.size() == 0)
		return true;

	// go through all sections
	for (std::vector<nbt::NBTTag*>::const_iterator it = tagSections->payload.begin();
	        it != tagSections->payload.end(); ++it) {
		nbt::TagCompound* tagSection = (nbt::TagCompound*) *it;
		nbt::TagByte* y = tagSection->findTag<nbt::TagByte>("Y", nbt::TAG_BYTE);
		nbt::TagByteArray* blocks = tagSection->findTag<nbt::TagByteArray>("Blocks", nbt::TAG_BYTE_ARRAY);
		nbt::TagByteArray* add = tagSection->findTag<nbt::TagByteArray>("Add", nbt::TAG_BYTE_ARRAY);
		nbt::TagByteArray* data = tagSection->findTag<nbt::TagByteArray>("Data", nbt::TAG_BYTE_ARRAY);
		// make sure section is valid
		if (y == NULL || blocks == NULL || data == NULL
				|| blocks->payload.size() != 4096
				|| data->payload.size() != 2048)
			continue;

		// add it
		ChunkSection section;
		section.y = y->payload;
		std::copy(blocks->payload.begin(), blocks->payload.end(), section.blocks);
		if (add == NULL || add->payload.size() != 2048)
			std::fill(&section.add[0], &section.add[2048], 0);
		else
			std::copy(add->payload.begin(), add->payload.end(), section.add);
		std::copy(data->payload.begin(), data->payload.end(), section.data);
		// set the position of this section
		section_offsets[section.y] = sections.size();
		sections.push_back(section);
	}

	return true;
}

/**
 * Clears the whole chunk data.
 */
void Chunk::clear() {
	sections.clear();
	for (int i = 0; i < 16; i++)
		section_offsets[i] = -1;
}

bool Chunk::hasSection(int section) const {
	return section_offsets[section] != -1;
}

void rotateBlockPos(int& x, int& z, int rotation) {
	int nx = x, nz = z;
	for (int i = 0; i < rotation; i++) {
		nx = z;
		nz = 15 - x;
		x = nx;
		z = nz;
	}
}

/**
 * Returns the block id at a position.
 */
uint16_t Chunk::getBlockID(const LocalBlockPos& pos) const {
	int section = pos.y / 16;
	if (section_offsets[section] == -1)
		return 0;
	// FIXME sometimes this happens, fix this
	if (sections.size() > 16 || sections.size() <= section_offsets[section]) {
		return 0;
	}

	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);

	int offset = ((pos.y % 16) * 16 + z) * 16 + x;
	uint16_t add = 0;
	if ((offset % 2) == 0)
		add = sections[section_offsets[section]].add[offset / 2] & 0xf;
	else
		add = (sections[section_offsets[section]].add[offset / 2] >> 4) & 0x0f;
	return sections[section_offsets[section]].blocks[offset] + (add << 8);
}

/**
 * Returns the block data at a position.
 */
uint8_t Chunk::getBlockData(const LocalBlockPos& pos) const {
	int section = pos.y / 16;
	if (section_offsets[section] == -1)
		return 0;

	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);

	int offset = ((pos.y % 16) * 16 + z) * 16 + x;
	if ((offset % 2) == 0)
		return sections[section_offsets[section]].data[offset / 2] & 0xf;
	return (sections[section_offsets[section]].data[offset / 2] >> 4) & 0x0f;
}

uint8_t Chunk::getBiomeAt(const LocalBlockPos& pos) const {
	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);

	return biomes[z * 16 + x];
}

//MCBlock::IsSolidBlock

bool Chunk::hasSolidBlock(const LocalBlockPos& pos) const
{
	uint16_t block_id = getBlockID(pos);
	if (block_id > 0)
	{
		if (MCBlock::IsSolidBlock(block_id))
			return true;
	}
	return false;
}

bool Chunk::hasBlock(const LocalBlockPos& pos, const uint16_t block_id, const uint8_t data, const uint8_t state) const
{
	bool hasBlock = true;
	uint16_t real_block_id = getBlockID(pos);
	if (real_block_id == 0)
		return false;

	if (block_id == 0)
		return true;

	if (real_block_id != block_id)
		return false;

	if (data == 255)
		return true;

	uint16_t real_data = getBlockData(pos);
	if (real_data != data)
		return false;

	/*if (state != 255)
	{
		uint16_t real_state = getBlockState(pos);
		if (real_state != state)
			hasBlock = false;
	}*/
	return hasBlock;
}

bool Chunk::GetBlockInfo(LocalBlockPos pos, uint16_t &block_id, uint8_t &data, uint8_t &state)
{
	uint16_t real_block_id = getBlockID(pos);
	if (real_block_id == 0)
		return false;
	block_id = real_block_id;
	data = getBlockData(pos);
	return true;
}

//
///*
//53(oak_stairs),67(stone_stairs),108(brick_stairs),109(stone_brick_stairs),114(nether_brick_stairs),128(sandstone_stairs),134(spruce_stairs),135(birch_stairs),
//136(jungle_stairs),156(quartz_stairs),163(acacia_stairs),164(dark_oak_stairs),180(red_sandstone_stairs)
//*/
//uint8_t Chunk::getStairsBlockState(const LocalBlockPos& pos, uint16_t block_id) const
//{
//	uint8_t state = 0;
//	uint8_t data = getBlockData(pos);
//	// facing "x-"
//	if (data == 0)
//	{
//		
//		if (hasBlock(LocalBlockPos(pos.x + 1, pos.z, pos.y), block_id, 2))        // state:outter 
//		{
//			state = 0;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x + 1, pos.z, pos.y), block_id, 3))   // state:outter 
//		{
//			state = 1;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x - 1, pos.z, pos.y), block_id, 2))   // state:inner 
//		{
//			state = 2;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x - 1, pos.z, pos.y), block_id, 3))   // state:inner
//		{
//			state = 3;
//		}
//		else
//			state = 7;
//		
//	}
//
//	// facing "x+"
//	if (data == 1)
//	{
//		if (hasBlock(LocalBlockPos(pos.x - 1, pos.z, pos.y), block_id, 2))   // state:outter 
//		{
//			state = 0;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x - 1, pos.z, pos.y), block_id, 3))   // state:outter
//		{
//			state = 1;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x + 1, pos.z, pos.y), block_id, 2))        // state:inner 
//		{
//			state = 2;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x + 1, pos.z, pos.y), block_id, 3))   // state:inner 
//		{
//			state = 3;
//		}
//		else
//			state = 7;
//	}
//
//	// facing "z-"
//	if (data == 2)
//	{
//		if (hasBlock(LocalBlockPos(pos.x, pos.z + 1, pos.y), block_id, 0))        // state:outter 
//		{
//			state = 0;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x, pos.z + 1, pos.y), block_id, 1))   // state:outter 
//		{
//			state = 1;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x, pos.z - 1, pos.y), block_id, 0))   // state:inner 
//		{
//			state = 2;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x, pos.z - 1, pos.y), block_id, 1))   // state:inner
//		{
//			state = 3;
//		}
//		else
//			state = 7;
//
//	}
//
//	// facing "z+"
//	if (data == 3)
//	{
//		if (hasBlock(LocalBlockPos(pos.x, pos.z - 1, pos.y), block_id, 0))   // state:outter 
//		{
//			state = 0;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x, pos.z - 1, pos.y), block_id, 1))   // state:outter
//		{
//			state = 1;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x, pos.z + 1, pos.y), block_id, 0))        // state:inner 
//		{
//			state = 2;
//		}
//		else if (hasBlock(LocalBlockPos(pos.x, pos.z + 1, pos.y), block_id, 1))   // state:inner 
//		{
//			state = 3;
//		}
//		else
//			state = 7;
//	}
//	return state;
//}
//
//uint8_t Chunk::getBlockState(const LocalBlockPos& pos) const{
//	uint8_t state = 0;
//	uint16_t block_id = getBlockID(pos);
//	if (block_id == 53)
//	{
//		state = getStairsBlockState(pos, block_id);
//	}
//	return state;
//}

const ChunkPos& Chunk::getPos() const {
	return pos;
}

}
}
