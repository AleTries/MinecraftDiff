#include "world/dimension_data.h"
#include "control.h"
#include "utils/unknown_recorder.h"
#include "world/common.h"
#include "world/misc.h"
#include "world/point_conversion.h"
#include "global.h"
#include "nbt.h"
#include "utils/fs.h"
#include "minecraft/v2/biome.h"
#include "minecraft/v2/block.h"

#include <random>
#include <fstream>

namespace
{
    bool vectorContains(const std::vector<int>& v, int32_t i)
    {
        for (const auto& iter : v) {
            if (iter == i) {
                return true;
            }
        }
        return false;
    }
}

namespace
{
    using mcpe_viz::MAX_BLOCK_HEIGHT;
    using mcpe_viz::kColorDefault;
    using mcpe_viz::local_htobe32;


    // note: super super old hsl2rgb code; origin unknown
    double _hue_to_rgb(double m1, double m2, double h) {
        while (h < 1.0) { h += 1.0; }
        while (h > 1.0) { h -= 1.0; }
        if ((h * 6.0) < 1.0) {
            return m1 + (m2 - m1) * h * 6.0;
        }
        if ((h * 2.0) < 1.0) {
            return m2;
        }
        if ((h * 3.0) < 2.0) {
            return m1 + (m2 - m1) * (2.0 / 3.0 - h) * 6.0;
        }
        return m1;
    }


    int32_t _clamp(int32_t v, int32_t minv, int32_t maxv) {
        if (v < minv) return minv;
        if (v > maxv) return maxv;
        return v;
    }


    int32_t hsl2rgb(double h, double s, double l, int32_t& r, int32_t& g, int32_t& b) {
        double m2;
        if (l <= 0.5) {
            m2 = l * (s + 1.0);
        }
        else {
            m2 = l + s - l * s;
        }
        double m1 = l * 2.0 - m2;
        double tr = _hue_to_rgb(m1, m2, h + 1.0 / 3.0);
        double tg = _hue_to_rgb(m1, m2, h);
        double tb = _hue_to_rgb(m1, m2, h - 1.0 / 3.0);
        r = _clamp((int)(tr * 255.0), 0, 255);
        g = _clamp((int)(tg * 255.0), 0, 255);
        b = _clamp((int)(tb * 255.0), 0, 255);
        return 0;
    }


    int32_t makeHslRamp(int32_t* pal, int32_t start, int32_t stop, double h1, double h2, double s1, double s2, double l1, double l2) {
        double steps = stop - start + 1;
        double dh = (h2 - h1) / steps;
        double ds = (s2 - s1) / steps;
        double dl = (l2 - l1) / steps;
        double h = h1, s = s1, l = l1;
        int32_t r, g, b;
        for (int32_t i = start; i <= stop; i++) {
            hsl2rgb(h, s, l, r, g, b);
            int32_t c = ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
            pal[i] = c;
            h += dh;
            s += ds;
            l += dl;
        }
        return 0;
    }


    struct Palette {
        Palette()
        {
            memset(this->value, 0, sizeof(this->value));
            // create red-green ramp; red to black and then black to green
            makeHslRamp(this->value, 0, 61, 0.0, 0.0, 0.9, 0.9, 0.8, 0.1);
            makeHslRamp(this->value, 63, MAX_BLOCK_HEIGHT, 0.4, 0.4, 0.9, 0.9, 0.1, 0.8);
            // force 62 (sea level) to gray
            this->value[62] = 0x303030;

            // fill 128..255 with purple (we should never see this color)
            for (int32_t i = (MAX_BLOCK_HEIGHT + 1); i < 256; i++) {
                this->value[i] = kColorDefault;
            }

            // convert palette
            for (int32_t i = 0; i < 256; i++) {
                this->value[i] = local_htobe32(this->value[i]);
            }
        }
        int32_t value[256];
    };

    Palette& get_palette()
    {
        static Palette instance;
        return instance;
    }
}

namespace mcpe_viz {
    void DimensionData_LevelDB::updateFastLists()
    {
        for (int32_t bid = 0; bid < 1024; bid++) {
            fastBlockHideList[bid] = vectorContains(blockHideList, bid);
            fastBlockForceTopList[bid] = vectorContains(blockForceTopList, bid);
            fastBlockToGeoJSONList[bid] = vectorContains(blockToGeoJSONList, bid);
        }
    }

    bool DimensionData_LevelDB::checkDoForDim(const std::vector<int>& v) const
    {
        if (std::find(v.begin(), v.end(), dimId) != v.end()){
            return true;
        }
        return false;
    }

    int32_t DimensionData_LevelDB::generateImage(const std::string& fname, const ImageModeType imageMode)
    {
        const int32_t chunkOffsetX = -minChunkX;
        const int32_t chunkOffsetZ = -minChunkZ;

        const int32_t chunkW = (maxChunkX - minChunkX + 1);
        const int32_t chunkH = (maxChunkZ - minChunkZ + 1);
        const int32_t imageW = chunkW * 16;
        const int32_t imageH = chunkH * 16;

        int32_t bpp = 3;
        bool rgbaFlag = false;
        uint8_t lut[256];

        if (imageMode == kImageModeHeightColAlpha) {
            bpp = 4;
            rgbaFlag = true;
            // todobig - experiment with other ways to do this lut for height alpha
            double vmax = (double)MAX_BLOCK_HEIGHT * (double)MAX_BLOCK_HEIGHT;
            for (int32_t i = 0; i <= MAX_BLOCK_HEIGHT; i++) {
                // todobig make the offset (32) a cmdline param
                double ti = ((MAX_BLOCK_HEIGHT + 1) + 32) - i;
                double v = ((double)(ti * ti) / vmax) * 255.0;
                if (v > 235.0) { v = 235.0; }
                if (v < 0.0) { v = 0.0; }
                lut[i] = uint8_t(v);
            }
        }

        // todohere -- reddit user silvergoat77 has a 1gb (!) world and it is approx 33k x 26k -- alloc chokes on this.
        // the solution is to write a chunk of rows at a time instead of the whole image...
        // but -- the code below is optimized to just iterate through the list and do it's thing instead of searching for each chunk
        // so --- we need to test before / after changing this to step thru in Z/X order

        // note RGB pixels
        uint8_t* buf = new uint8_t[imageW * 16 * bpp];

        uint8_t* rows[16];
        for (int i = 0; i < 16; i++) {
            rows[i] = &buf[i * imageW * bpp];
        }

        int32_t color;
        const char* pcolor;
        if (bpp == 4) {
            pcolor = (const char*)&color;
        }
        else {
            const char* pcolor_temp = (const char*)&color;
            pcolor = &pcolor_temp[1];
        }

        PngWriter png;
        if (outputPNG_init(png, fname, makeImageDescription(imageMode, 0), imageW, imageH, rgbaFlag) != 0) {
            delete[] buf;
            return -1;
        }

        for (int32_t iz = 0, chunkZ = minChunkZ; iz < imageH; iz += 16, chunkZ++) {

            // clear buffer
            memset(buf, 0, imageW * 16 * bpp);

            for (int32_t ix = 0, chunkX = minChunkX; ix < imageW; ix += 16, chunkX++) {

                ChunkKey chunkKey(chunkX, chunkZ);
                if (!chunks_has_key(chunks, chunkKey)) {
                    continue;
                }

                const auto& it = chunks.at(chunkKey);

                int32_t imageX = (it->chunkX + chunkOffsetX) * 16;
                int32_t imageZ = (it->chunkZ + chunkOffsetZ) * 16;

                int32_t worldX = it->chunkX * 16;
                int32_t worldZ = it->chunkZ * 16;

                for (int32_t cz = 0; cz < 16; cz++) {
                    for (int32_t cx = 0; cx < 16; cx++) {

                        // todobig - we could do EVERYTHING (but initial key scan) in one pass:
                        //   do images here, then iterate over chunkspace again looking for items that populate geojson list

                        // todo - this big conditional inside an inner loop, not so good

                        if (imageMode == kImageModeBiome) {
                            // get biome color
                            int32_t biomeId = it->grassAndBiome[cx][cz] & 0xff;

                            auto biome = Biome::get(biomeId);
                            if (biome != nullptr) {
                                color = biome->color();
                            }
                            else {
                                log::trace("Unknown biome {} 0x{:x}", biomeId, biomeId);
                                record_unknown_biome_id(biomeId);
                                color = local_htobe32(0xff2020);
                            }
                        }
                        else if (imageMode == kImageModeGrass) {
                            // get grass color
                            int32_t grassColor = it->grassAndBiome[cx][cz] >> 8;
                            color = local_htobe32(grassColor);
                        }
                        else if (imageMode == kImageModeHeightCol) {
                            // get height value and use red-black-green palette
                            if (control.heightMode == kHeightModeTop) {
                                uint8_t c = it->topBlockY[cx][cz];
                                //color = palRedBlackGreen[c];
                                color = get_palette().value[c];
                            }
                            else {
                                uint8_t c = it->heightCol[cx][cz];
                                color = get_palette().value[c];
                            }
                        }
                        else if (imageMode == kImageModeHeightColGrayscale) {
                            // get height value and make it grayscale
                            if (control.heightMode == kHeightModeTop) {
                                uint8_t c = it->topBlockY[cx][cz];
                                color = (c << 24) | (c << 16) | (c << 8);
                            }
                            else {
                                uint8_t c = it->heightCol[cx][cz];
                                color = (c << 24) | (c << 16) | (c << 8);
                            }
                        }
                        else if (imageMode == kImageModeHeightColAlpha) {
                            // get height value and make it alpha
                            uint8_t c;
                            if (control.heightMode == kHeightModeTop) {
                                c = it->topBlockY[cx][cz];
                            }
                            else {
                                c = it->heightCol[cx][cz];
                            }
                            // c = (90 - (int32_t)it->heightCol[cx][cz]) * 2;
                            c = lut[c];
                            color = ((c & 0xff) << 24);
                        }
                        else if (imageMode == kImageModeBlockLight) {
                            // get block light value and expand it (is only 4-bits)
                            uint8_t c = (it->topLight[cx][cz] & 0x0f) << 4;
                            color = (c << 24) | (c << 16) | (c << 8);
                        }
                        else if (imageMode == kImageModeSkyLight) {
                            // get sky light value and expand it (is only 4-bits)
                            uint8_t c = (it->topLight[cx][cz] & 0xf0);
                            color = (c << 24) | (c << 16) | (c << 8);
                        }
                        else {
                            // regular image
                            int32_t blockid = it->blocks[cx][cz];
                            auto block = Block::get(blockid);
                            if (block != nullptr) {

                                if (block->hasVariants()) {
                                    // we need to get blockdata
                                    int32_t blockdata = it->data[cx][cz];
                                    auto variant = block->getVariantByBlockData(blockdata);
                                    if (variant != nullptr) {
                                        color = variant->color();
                                    }
                                    else {
                                        record_unknown_block_variant(
                                            blockid,
                                            block->name,
                                            blockdata);
                                        // since we did not find the variant, use the parent block's color
                                        color = block->color();
                                    }
                                }
                                else {
                                    color = block->color();
                                    if (!block->is_color_set()) {
                                        block->color_set_need_count += 1;
                                    }
                                }
                            }
                            else {
                                record_unknown_block_id(blockid);
                                color = kColorDefault;
                            }
                        }

                        // do grid lines
                        if (checkDoForDim(control.doGrid) && (cx == 0 || cz == 0)) {
                            if ((it->chunkX == 0) && (it->chunkZ == 0) && (cx == 0) && (cz == 0)) {
                                color = local_htobe32(0xeb3333);
                            }
                            else {
                                color = local_htobe32(0xc1ffc4);
                            }
                        }

#ifdef PIXEL_COPY_MEMCPY
                        memcpy(&buf[((cz)*imageW + (imageX + cx)) * bpp], pcolor, bpp);
#else
                        // todobig - support for bpp here
// todo - any use in optimizing the offset calc?
                        buf[((cz)*imageW + (imageX + cx)) * 3] = pcolor[1];
                        buf[((cz)*imageW + (imageX + cx)) * 3 + 1] = pcolor[2];
                        buf[((cz)*imageW + (imageX + cx)) * 3 + 2] = pcolor[3];
#endif

                        // report interesting coordinates
                        if (dimId == kDimIdOverworld && imageMode == kImageModeTerrain) {
                            int32_t tix = (imageX + cx);
                            int32_t tiz = (imageZ + cz);
                            int32_t twx = (worldX + cx);
                            int32_t twz = (worldZ + cz);
                            if ((twx == 0) && (twz == 0)) {
                                log::info("    Info: World (0, 0) is at image ({}, {})", tix, tiz);
                            }
                            // todobig - just report this somwhere instead of having to pass the spawn params
                            if (twx == worldSpawnX && twz == worldSpawnZ) {
                                log::info("    Info: World Spawn ({}, {}) is at image ({}, {})",
                                    worldSpawnX, worldSpawnZ, tix, tiz);
                            }
                        }
                    }
                }
            }
            // write rows
            outputPNG_writeRows(png, rows, 16);
        }

        // output the image
        outputPNG_close(png);

        delete[] buf;

        // report items that need to have their color set properly (in the XML file)
        if (imageMode == kImageModeTerrain) {
            for(auto& i: Block::list()) {
                if (i->color_set_need_count != 0) {
                    log::info("    Need pixel color for: 0x{:x} '{}' (count={})",
                        i->id, i->name, i->color_set_need_count);
                }
            }
        }
        return 0;
    }

    bool DimensionData_LevelDB::isSlimeChunk_MCPE(int32_t cX, int32_t cZ)
    {
        //
            // MCPE slime-chunk checker
            // From Minecraft: Pocket Edition 0.15.0 (0.15.0.50_V870150050)
            // Reverse engineered by @protolambda and @jocopa3
            //
            // NOTE:
            // - The world-seed doesn't seem to be incorporated into the randomness, which is very odd.
            //   This means that every world has its slime-chunks in the exact same chunks!
            //   This is not officially confirmed yet.
            // - Reverse engineering this code cost a lot of time,
            //   please add CREDITS when you are copying this.
            //   Copy the following into your program source:
            //     MCPE slime-chunk checker; reverse engineered by @protolambda and @jocopa3
            //

            // chunkX/Z are the chunk-coordinates, used in the DB keys etc.
            // Unsigned int32, using 64 bit longs to work-around the sign issue.
        int64_t chunkX_uint = cX & 0xffffffffL;
        int64_t chunkZ_uint = cZ & 0xffffffffL;

        // Combine X and Z into a 32 bit int (again, long to work around sign issue)
        int64_t seed = (chunkX_uint * 0x1f1f1f1fL) ^ chunkZ_uint;

        // The random function MCPE uses, not the same as MCPC!
        // This is a Mersenne Twister; MT19937 by Takuji Nishimura and Makoto Matsumoto.
        // Java adaption source: http://dybfin.wustl.edu/teaching/compufinj/MTwister.java
        //MTwister random = new MTwister();
        std::mt19937 random;
        //random.init_genrand(seed);
        random.seed(unsigned(seed));

        // The output of the random function, first operand of the asm umull instruction
        //int64_tn = random.genrand_int32();
        int64_t n = random();

        // The other operand, magic bit number that keeps characteristics
        // In binary: 1100 1100 1100 1100 1100 1100 1100 1101
        int64_t m = 0xcccccccdL;

        // umull (unsigned long multiplication)
        // Stores the result of multiplying two int32 integers in two registers (lo and hi).
        // Java workaround: store the result in a 64 bit long, instead of two 32 bit registers.
        int64_t product = n * m;

        // The umull instruction puts the result in a lo and a hi register, the lo one is not used.
        int64_t hi = (product >> 32) & 0xffffffffL;

        // Make room for 3 bits, preparation for decrease of randomness by a factor 10.
        int64_t hi_shift3 = (hi >> 0x3) & 0xffffffffL;

        // Multiply with 10 (3 bits)
        // ---> effect: the 3 bit randomness decrease expresses a 1 in a 10 chance.
        int64_t res = (((hi_shift3 + (hi_shift3 * 0x4)) & 0xffffffffL) * 0x2) & 0xffffffffL;

        // Final check: is the input equal to 10 times less random, but comparable, output.
        // Every chunk has a 1 in 10 chance to be a slime-chunk.
        return n == res;
    }

    int32_t DimensionData_LevelDB::generateSlices(leveldb::DB* db, const std::string& fnBase)
    {
        const int32_t chunkOffsetX = -minChunkX;
        const int32_t chunkOffsetZ = -minChunkZ;

        const int32_t chunkW = (maxChunkX - minChunkX + 1);
        const int32_t chunkH = (maxChunkZ - minChunkZ + 1);
        const int32_t imageW = chunkW * 16;
        const int32_t imageH = chunkH * 16;

        char keybuf[128];
        int32_t keybuflen;
        int32_t kw = dimId;
        uint8_t kt = 0x30;
        uint8_t kt_v3 = 0x2f;
        leveldb::Status dstatus;

        log::info("    Writing all images in one pass");

        std::string svalue;

        int32_t color;
        const char* pcolor = (const char*)&color;

        int16_t* emuchunk = new int16_t[NUM_BYTES_CHUNK_V3];

        // create png helpers
        PngWriter png[MAX_BLOCK_HEIGHT + 1];
        for (int32_t cy = 0; cy <= MAX_BLOCK_HEIGHT; cy++) {
            std::string fnameTmp = fnBase + ".slice.full.";
            fnameTmp += name;
            fnameTmp += ".";
            sprintf(keybuf, "%03d", cy);
            fnameTmp += keybuf;
            fnameTmp += ".png";

            control.fnLayerRaw[dimId][cy] = fnameTmp;

            if (png[cy].init(fnameTmp, makeImageDescription(-1, cy), imageW, imageH, 16, false, true) != 0) {
                delete[] emuchunk;
                return -1;
            }
        }

        // create row buffers
        uint8_t* rbuf[MAX_BLOCK_HEIGHT + 1];
        for (int32_t cy = 0; cy <= MAX_BLOCK_HEIGHT; cy++) {
            rbuf[cy] = new uint8_t[(imageW * 3) * 16];
            // setup row pointers
            for (int32_t cz = 0; cz < 16; cz++) {
                png[cy].row_pointers[cz] = &rbuf[cy][(cz * imageW) * 3];
            }
        }

        // create a helper buffer which contains topBlockY for the entire image
        uint8_t currTopBlockY = MAX_BLOCK_HEIGHT;
        size_t bsize = (size_t)imageW * (size_t)imageH;
        uint8_t* tbuf = new uint8_t[bsize];
        memset(tbuf, MAX_BLOCK_HEIGHT, bsize);
        for (const auto& it : chunks) {
            int32_t ix = (it.second->chunkX + chunkOffsetX) * 16;
            int32_t iz = (it.second->chunkZ + chunkOffsetZ) * 16;
            for (int32_t cz = 0; cz < 16; cz++) {
                for (int32_t cx = 0; cx < 16; cx++) {
                    tbuf[(iz + cz) * imageW + (ix + cx)] = it.second->topBlockY[cx][cz];
                }
            }
        };

        int32_t foundCt = 0, notFoundCt2 = 0;
        //todozooz -- new 16-bit block-id's (instead of 8-bit) are a BIG issue - this needs attention here
        uint8_t blockdata;
        int32_t blockid;

        // we operate on sets of 16 rows (which is one chunk high) of image z
        int32_t runCt = 0;
        for (int32_t imageZ = 0, chunkZ = minChunkZ; imageZ < imageH; imageZ += 16, chunkZ++) {

            if ((runCt++ % 20) == 0) {
                log::info("    Row {} of {}", imageZ, imageH);
            }

            for (int32_t imageX = 0, chunkX = minChunkX; imageX < imageW; imageX += 16, chunkX++) {

                // FIRST - we try pre-0.17 chunks

                // construct key to get the chunk
                if (dimId == kDimIdOverworld) {
                    //overworld
                    memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
                    memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
                    memcpy(&keybuf[8], &kt, sizeof(uint8_t));
                    keybuflen = 9;
                }
                else {
                    // nether (and probably any others that are added)
                    memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
                    memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
                    memcpy(&keybuf[8], &kw, sizeof(int32_t));
                    memcpy(&keybuf[12], &kt, sizeof(uint8_t));
                    keybuflen = 13;
                }

                dstatus = db->Get(levelDbReadOptions, leveldb::Slice(keybuf, keybuflen), &svalue);
                if (dstatus.ok()) {

                    // we got a pre-0.17 chunk
                    const char* ochunk = nullptr;
                    const char* pchunk = nullptr;

                    pchunk = svalue.data();
                    ochunk = pchunk;
                    // size_t ochunk_size = svalue.size();
                    foundCt++;

                    // we step through the chunk in the natural order to speed things up
                    for (int32_t cx = 0; cx < 16; cx++) {
                        for (int32_t cz = 0; cz < 16; cz++) {
                            currTopBlockY = tbuf[(imageZ + cz) * imageW + imageX + cx];
                            for (int32_t cy = 0; cy <= MAX_BLOCK_HEIGHT_127; cy++) {

                                // todo - if we use this, we get blockdata errors... somethings not right
                                //blockid = *(pchunk++);
                                blockid = getBlockId_LevelDB_v2(ochunk, cx, cz, cy);

                                if (blockid == 0 && (cy > currTopBlockY) && (dimId != kDimIdNether)) {

                                    // special handling for air -- keep existing value if we are above top block
                                    // the idea is to show air underground, but hide it above so that the map is not all black pixels @ y=MAX_BLOCK_HEIGHT
                                    // however, we do NOT do this for the nether. because: the nether

                                    // we need to copy this pixel from another layer
                                    memcpy(&rbuf[cy][((cz * imageW) + imageX + cx) * 3],
                                        &rbuf[currTopBlockY][((cz * imageW) + imageX + cx) * 3],
                                        3);

                                }
                                else {
                                    auto block = Block::get(blockid);
                                    if (block != nullptr) {
                                        if (block->hasVariants()) {
                                            blockdata = getBlockData_LevelDB_v2(ochunk, cx, cz, cy);
                                            auto variant = block->getVariantByBlockData(blockdata);
                                            if (variant != nullptr) {
                                                color = variant->color();
                                            }
                                            else {
                                                record_unknown_block_variant(
                                                    block->id,
                                                    block->name,
                                                    blockdata);
                                                // since we did not find the variant, use the parent block's color
                                                color = block->color();
                                            }
                                        }
                                        else {
                                            color = block->color();
                                        }
                                    }
                                    else {
                                        record_unknown_block_id(blockid);
                                        color = kColorDefault;
                                    }

#ifdef PIXEL_COPY_MEMCPY
                                    memcpy(&rbuf[cy][((cz * imageW) + imageX + cx) * 3], &pcolor[1], 3);
#else
                                    // todo - any use in optimizing the offset calc?
                                    rbuf[cy][((cz * imageW) + imageX + cx) * 3] = pcolor[1];
                                    rbuf[cy][((cz * imageW) + imageX + cx) * 3 + 1] = pcolor[2];
                                    rbuf[cy][((cz * imageW) + imageX + cx) * 3 + 2] = pcolor[3];
#endif
                                }
                            }

                            // to support 256h worlds, for v2 chunks, we need to make 128..255 the same as 127
                            // todo - could optimize this
                            for (int cy = 128; cy <= MAX_BLOCK_HEIGHT; cy++) {
                                memcpy(&rbuf[cy][((cz * imageW) + imageX + cx) * 3],
                                    &rbuf[127][((cz * imageW) + imageX + cx) * 3], 3);
                            }

                        }
                    }
                }
                else {

                    // we did NOT find a pre-0.17 chunk...

                    // SECOND -- we try post 0.17 chunks

                    // we need to iterate over all possible y cubic chunks here...
                    int32_t cubicFoundCount = 0;
                    for (int8_t cubicy = 0; cubicy < MAX_CUBIC_Y; cubicy++) {

                        // todobug - this fails around level 112? on another1 -- weird -- run a valgrind to see where we're messing up
                        //check valgrind output

                        // construct key to get the chunk
                        if (dimId == kDimIdOverworld) {
                            //overworld
                            memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
                            memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
                            memcpy(&keybuf[8], &kt_v3, sizeof(uint8_t));
                            memcpy(&keybuf[9], &cubicy, sizeof(uint8_t));
                            keybuflen = 10;
                        }
                        else {
                            // nether (and probably any others that are added)
                            memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
                            memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
                            memcpy(&keybuf[8], &kw, sizeof(int32_t));
                            memcpy(&keybuf[12], &kt_v3, sizeof(uint8_t));
                            memcpy(&keybuf[13], &cubicy, sizeof(uint8_t));
                            keybuflen = 14;
                        }

                        dstatus = db->Get(levelDbReadOptions, leveldb::Slice(keybuf, keybuflen), &svalue);
                        if (dstatus.ok()) {
                            cubicFoundCount++;

                            // we got a post-0.17 cubic chunk
                            const char* rchunk = svalue.data();
                            const int16_t* pchunk_word = (int16_t*)svalue.data();
                            const char* pchunk_byte = (char*)svalue.data();
                            size_t ochunk_size = svalue.size();
                            const int16_t* ochunk_word = pchunk_word;
                            const char* ochunk_byte = pchunk_byte;
                            bool wordModeFlag = false;
                            foundCt++;

                            // determine if it is a v7 chunk and process accordingly
                            //todozooz - here is where it gets weird
                            if (rchunk[0] != 0x0) {
                                // we have a v7 chunk - emulate v3
                                convertChunkV7toV3(rchunk, ochunk_size, emuchunk);
                                wordModeFlag = true;
                                pchunk_word = emuchunk;
                                ochunk_word = emuchunk;
                                ochunk_size = NUM_BYTES_CHUNK_V3;
                            }
                            else {
                                wordModeFlag = false;
                                // slogger.msg(kLogWarning,"Found a non-v7 chunk\n");
                            }

                            // the first byte is not interesting to us (it is version #?)
                            pchunk_word++;
                            pchunk_byte++;

                            // we step through the chunk in the natural order to speed things up
                            for (int32_t cx = 0; cx < 16; cx++) {
                                for (int32_t cz = 0; cz < 16; cz++) {
                                    currTopBlockY = tbuf[(imageZ + cz) * imageW + imageX + cx];
                                    for (int32_t ccy = 0; ccy < 16; ccy++) {
                                        int32_t cy = cubicy * 16 + ccy;

                                        // todo - if we use this, we get blockdata errors... somethings not right
                                        if (wordModeFlag) {
                                            blockid = *(pchunk_word++);
                                        }
                                        else {
                                            //todozooz - getting blockid manually fixes issue
                                            // blockid = *(pchunk_byte++);
                                            blockid = getBlockId_LevelDB_v3(ochunk_byte, cx, cz, ccy);
                                        }

                                        // blockid = getBlockId_LevelDB_v3(ochunk, cx,cz,ccy);

                                        if (blockid == 0 && (cy > currTopBlockY) && (dimId != kDimIdNether)) {

                                            // special handling for air -- keep existing value if we are above top block
                                            // the idea is to show air underground, but hide it above so that the map is not all black pixels @ y=MAX_BLOCK_HEIGHT
                                            // however, we do NOT do this for the nether. because: the nether

                                            // we need to copy this pixel from another layer
                                            memcpy(&rbuf[cy][((cz * imageW) + imageX + cx) * 3],
                                                &rbuf[currTopBlockY][((cz * imageW) + imageX + cx) * 3],
                                                3);

                                        }
                                        else {
                                            // TODO not safe 
                                            if (blockid >= 0 && blockid < 1024) {
                                                auto block = Block::get(blockid);
                                                if (block != nullptr) {
                                                    if (block->hasVariants()) {
                                                        if (wordModeFlag) {
                                                            blockdata = getBlockData_LevelDB_v3__fake_v7(ochunk_word,
                                                                ochunk_size,
                                                                cx, cz, ccy);
                                                        }
                                                        else {
                                                            blockdata = getBlockData_LevelDB_v3(ochunk_byte,
                                                                ochunk_size, cx, cz,
                                                                ccy);
                                                        }
                                                        auto variant = block->getVariantByBlockData(blockdata);
                                                        if (variant != nullptr) {
                                                            color = variant->color();
                                                        }
                                                        else {
                                                            record_unknown_block_variant(
                                                                block->id,
                                                                block->name,
                                                                blockdata);
                                                            color = block->color();
                                                        }
                                                    }
                                                    else {
                                                        color = block->color();
                                                    }
                                                }
                                                else {
                                                    record_unknown_block_id(blockid);
                                                    color = kColorDefault;
                                                }

                                            }
                                            else {
                                                // bad blockid
                                                //todozooz todostopper - we get a lot of these w/ negative blockid around row 4800 of world 'another1'
                                                log::trace("Invalid blockid={} (image {} {}) (cc {} {} {})",
                                                    blockid, imageX, imageZ, cx, cz, cy);
                                                record_unknown_block_id(blockid);
                                                // set an unused color
                                                color = local_htobe32(0xf010d0);
                                            }

#ifdef PIXEL_COPY_MEMCPY
                                            memcpy(&rbuf[cy][((cz * imageW) + imageX + cx) * 3], &pcolor[1], 3);
#else
                                            // todo - any use in optimizing the offset calc?
                                            rbuf[cy][((cz * imageW) + imageX + cx) * 3] = pcolor[1];
                                            rbuf[cy][((cz * imageW) + imageX + cx) * 3 + 1] = pcolor[2];
                                            rbuf[cy][((cz * imageW) + imageX + cx) * 3 + 2] = pcolor[3];
#endif
                                        }
                                    }
                                }
                            }
                        }
                        else {
                            // we did NOT find the cubic chunk, which means that it is 100% air

                            for (int32_t cx = 0; cx < 16; cx++) {
                                for (int32_t cz = 0; cz < 16; cz++) {
                                    currTopBlockY = tbuf[(imageZ + cz) * imageW + imageX + cx];
                                    for (int32_t ccy = 0; ccy < 16; ccy++) {
                                        int32_t cy = cubicy * 16 + ccy;
                                        if ((cy > currTopBlockY) && (dimId != kDimIdNether)) {
                                            // special handling for air -- keep existing value if we are above top block
                                            // the idea is to show air underground, but hide it above so that the map is not all black pixels @ y=MAX_BLOCK_HEIGHT
                                            // however, we do NOT do this for the nether. because: the nether

                                            // we need to copy this pixel from another layer
                                            memcpy(&rbuf[cy][((cz * imageW) + imageX + cx) * 3],
                                                &rbuf[currTopBlockY][((cz * imageW) + imageX + cx) * 3],
                                                3);
                                        }
                                        else {
                                            memset(&rbuf[cy][((cz * imageW) + imageX + cx) * 3], 0, 3);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (cubicFoundCount <= 0) {

                        // FINALLY -- we did not find the chunk at all
                        notFoundCt2++;
                        // slogger.msg(kLogInfo1,"WARNING: Did not find chunk in leveldb x=%d z=%d status=%s\n", chunkX, chunkZ, dstatus.ToString().c_str());

                        // we need to clear this area
                        for (int32_t cy = 0; cy <= MAX_BLOCK_HEIGHT; cy++) {
                            for (int32_t cz = 0; cz < 16; cz++) {
                                memset(&rbuf[cy][((cz * imageW) + imageX) * 3], 0, 16 * 3);
                            }
                        }
                        // todonow - need this?
                        //continue;
                    }
                }

            }

            // put the png rows
            // todo - png lib is SLOW - worth it to alloc a larger window (16-row increments) and write in batches?
            for (int32_t cy = 0; cy <= MAX_BLOCK_HEIGHT; cy++) {
                png_write_rows(png[cy].png, png[cy].row_pointers, 16);
            }
        }

        for (int32_t cy = 0; cy <= MAX_BLOCK_HEIGHT; cy++) {
            delete[] rbuf[cy];
            png[cy].close();
        }

        delete[] tbuf;

        // slogger.msg(kLogInfo1,"    Chunk Info: Found = %d / Not Found (our list) = %d / Not Found (leveldb) = %d\n", foundCt, notFoundCt1, notFoundCt2);

        delete[] emuchunk;
        return 0;
    }


const int16_t* DimensionData_LevelDB::findChunk(leveldb::DB* db, DimensionType dimId, int32_t x, uint8_t y, int32_t z)
{
    char keybuf[128];
    int32_t keybuflen=0;
    int32_t kw = dimId;
    uint8_t kt_v3 = 0x2f;
    int32_t blockid = 1025;

    int32_t chunkX = x/16, chunkZ = z/16, cubicy = y/16;

    // construct key to get the chunk
    if (dimId == kDimIdOverworld) {
        //overworld
        memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
        memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
        memcpy(&keybuf[8], &kt_v3, sizeof(uint8_t));
        memcpy(&keybuf[9], &cubicy, sizeof(uint8_t));
        keybuflen = 10;
    }
    else {
        // nether (and probably any others that are added)
        memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
        memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
        memcpy(&keybuf[8], &kw, sizeof(int32_t));
        memcpy(&keybuf[12], &kt_v3, sizeof(uint8_t));
        memcpy(&keybuf[13], &cubicy, sizeof(uint8_t));
        keybuflen = 14;
    }

    leveldb::Status dstatus;
    auto emuchunk = new int16_t[NUM_BYTES_CHUNK_V3];
    int16_t * retPtr = nullptr;
    std::string svalue;
    dstatus = db->Get(levelDbReadOptions, leveldb::Slice(keybuf, keybuflen), &svalue);
    if (dstatus.ok()) {
        // we got a post-0.17 cubic chunk
        const char* rchunk = svalue.data();

        // determine if it is a v7 chunk and process accordingly
        //todozooz - here is where it gets weird
        // we have a v7 chunk - emulate v3
        convertChunkV7toV3(rchunk, svalue.size(), emuchunk);

        retPtr = emuchunk;
    }

    return retPtr;
}

int32_t DimensionData_LevelDB::generateBlockList(leveldb::DB* db, const std::string& dimName, leveldb::DB* emptyDb)
{
    int32_t limMinX = minChunkX*16;

    if (control.minX != 0x8FFFFFFF)
    {
        if (control.minX/16 > minChunkX)
        {
           limMinX = control.minX;
       }
    }

    int32_t limMaxX = maxChunkX*16;
    if (control.maxX != 0x8FFFFFFF)
    {
        if (control.maxX/16 < maxChunkX)
        {
            limMaxX = control.maxX;
        }
    }

    int32_t limMinZ = minChunkZ*16;

    if (control.minZ != 0x8FFFFFFF)
    {
        if (control.minZ/16 > minChunkZ)
        {
           limMinZ = control.minZ;
       }
    }
    int32_t limMaxZ = maxChunkZ*16;
    if (control.maxZ != 0x8FFFFFFF)
    {
        if (control.maxZ/16 < maxChunkZ)
        {
            limMaxZ = control.maxZ;
        }
    }

    int32_t limMinY = 0;

    if (control.minY != 0x8FFFFFFF)
    {
        if (control.minY > 0)
        {
           limMinY = control.minY;
       }
    }
    int32_t limMaxY = 255;
    if (control.maxY != 0x8FFFFFFF)
    {
        if (control.maxY < 255)
        {
            limMaxY = control.maxY;
        }
    }

    const int32_t chunkW = (maxChunkX - minChunkX + 1);
    const int32_t chunkH = (maxChunkZ - minChunkZ + 1);
    const int32_t imageW = chunkW * 16;
    const int32_t imageH = chunkH * 16;

    unsigned int blockListCnt = 0;
    log::info("   World '{}' of size [X:{} => {}, Z:{} => {}]", control.dirLeveldb, 16*minChunkX, 16*maxChunkX, 16*minChunkZ, 16*maxChunkZ);
    log::info("   Scanning World within limits [X:{} => {}, Y:{} => {}, Z:{} => {}]", limMinX, limMaxX, limMinY, limMaxY, limMinZ, limMaxZ);
    std::ofstream fd;
    fd.open(control.dirLeveldb + "_"+ dimName+"_blocks.xyz");
    std::ofstream ld;
    ld.open(control.dirLeveldb+ "_"+ dimName+"_blocks.txt");
    ld << "WORLD NAME: '" << control.dirLeveldb << "'" << std::endl;
    if (emptyDb != nullptr)
    {
        ld << "COMPARISON WORLD (EMPTY): '" << control.emptyDbName << "'" << std::endl;
    }
    ld << "WORLD SIZE: [X:" << 16*minChunkX << " => " << 16*maxChunkX;
    ld << ", Z:" << 16*minChunkZ << " => " << 16*maxChunkZ << "]" << std::endl;
    ld << "WORLD FILTER: [X:" << limMinX << " => " << limMaxX;
    ld << ", Y:" << limMinY << " => " << limMaxY;
    ld << ", Z:" << limMinZ << " => " << limMaxZ << "]" << std::endl;
    ld << "WORLD BLOCKS FILTERED by name '" << control.blockFilter << "'" << std::endl;

    uint64_t blockCnt[1024] = {};
    struct Coords
    {
        int x, y, z;
    };
    std::vector<Coords> blockLists[1024];

    // we operate on sets of 16 rows (which is one chunk high) of image z
    int32_t runCt = 0;
    uint32_t worldChunksFound = 0;
    uint32_t emptyMatchChunks = 0;
    for (int8_t cubicy = 0; cubicy < MAX_CUBIC_Y; cubicy++)
    for (int32_t imageZ = 0, chunkZ = minChunkZ; imageZ < imageH; imageZ += 16, chunkZ++) {

        if ((runCt++ % 20) == 0) {
            log::info("    Row {} of {}", imageZ, imageH);
        }

        for (int32_t imageX = 0, chunkX = minChunkX; imageX < imageW; imageX += 16, chunkX++) {

           const int16_t *firstBlockId = findChunk(db, (DimensionType)dimId, 16*minChunkX + imageX, 16*cubicy, 16*minChunkZ + imageZ);
           if (firstBlockId != nullptr) {

                worldChunksFound++;
                // Check if we have a comparison (empty) world
                const int16_t *emptyBlockId = nullptr;
                if (emptyDb != nullptr)
                {
                    emptyBlockId = findChunk(emptyDb, (DimensionType)dimId, 16*minChunkX + imageX, 16*cubicy, 16*minChunkZ + imageZ);
                    if (emptyBlockId == nullptr)
                    {
                        // When doing a diff, skip unless the chunk exists in both worlds
                        delete[] firstBlockId;
                        continue;
                    }
                    emptyMatchChunks++;
                }

                // the first byte is not interesting to us (it is version #?)
                auto chunkPtr = &firstBlockId[1];
                const int16_t* emptyChunk = nullptr;
                if (emptyBlockId != nullptr)
                {
                   emptyChunk = &emptyBlockId[1];
                }

                // we step through the chunk in the natural order to speed things up
                for (int32_t cx = 0; cx < 16; cx++) {
                    for (int32_t cz = 0; cz < 16; cz++) {
                        for (int32_t cy = 0; cy < 16; cy++) {

                            int x = 16*minChunkX + imageX + cx;
                            int z = 16*minChunkZ + imageZ + cz;
                            int y = 16*cubicy + cy;
                            uint16_t blockid = *(chunkPtr++);
                            uint16_t emptyId = blockid+1;
                            if (emptyChunk != nullptr)
                            {
                                emptyId = *(emptyChunk++);
                            }

                            if ( (x >= limMinX) and (x <= limMaxX) and (z >= limMinZ) and (z <= limMaxZ) and (y >= limMinY) and (y <= limMaxY))
                            {
                                if (blockid < 1024)
                                {
                                    auto block = Block::get(blockid);
                                    if (block == nullptr) continue;

                                    if (emptyId == blockid)
                                    {
                                        // When doing a comparison, ignore identical bocks!
                                        continue;
                                    }

                                    if ((control.blockFilter == "<all>") or (block->name == control.blockFilter))
                                    {
                                        // Ignore air blocks in output point cloud
                                        if (blockid != 0)
                                        {
                                            uint32_t color = block->color();
                                            uint8_t r = (color >> 8) & 0xFF;
                                            uint8_t g = (color >> 16) & 0xFF;
                                            uint8_t b = (color >> 24) & 0xFF;
                                            fd << x << ", " << y << ", " << z << ", ";
                                            fd << (int16_t)r << ", " << (int16_t)g << ", " << (int16_t)b << std::endl;
                                        }
                                        if (blockListCnt < control.blockListMax)
                                        {
                                            blockListCnt++;
                                            ld << "blockid=" << std::dec << blockid  << ", name='" << block->name << "', (" << x << ", " << (int16_t)y << ", " << z << ")" << std::endl;
                                        }
                                    }

                                    blockCnt[blockid] += 1;

                                    if (blockCnt[blockid] <= control.blockListRare)
                                    {
                                       blockLists[blockid].push_back({x, y, z});
                                    }
                                }
                            }
                        }
                    }
                }
                delete [] firstBlockId;
                if (emptyBlockId != nullptr)
                {
                    delete [] emptyBlockId;
                }

            }
        }
    }

    if ((emptyMatchChunks != 0) and (worldChunksFound != 0))
    {
        log::info("    Found {}/{} comparison chunks", emptyMatchChunks, worldChunksFound);
        ld << "WORLD COMPARE CHUNKS " << emptyMatchChunks << "/" << worldChunksFound << " = ";
        ld << 100.0*emptyMatchChunks/worldChunksFound << "%" << std::endl;
    }

    // Sort array
    int arrIdx[1024] = {};
    for (int i=0; i<1024; i++) arrIdx[i]=i;

    // Define lambda function comparison
    auto compareForSort = [blockCnt] (int i1, int i2)
    {
        return (blockCnt[i1] < blockCnt[i2]);
    };

    // Sort array by number of blocks
    std::sort(arrIdx, arrIdx+1024, compareForSort);

    ld << "WORLD RARE BLOCKS (TOTAL less that " << control.blockListRare << ")" << std::endl;

    for (int i=0; i<1024; i++)
    {
        int idx = arrIdx[i];
        if (blockCnt[idx] <= control.blockListRare)
        {
            for(auto v : blockLists[idx])
            {
                auto block = Block::get(idx);
                ld << "blockid=" << std::dec << idx  << ", name='" << block->name << "', (" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
            }
        }
        blockLists[idx].clear();
    }

    uint32_t totCnt = 0;
    for (int i=0; i<1024; i++) totCnt+=blockCnt[i];
    ld << "WORLD BLOCKS LEGEND (TOTAL #= " << totCnt << ")" << std::endl;
    for (int i=0; i<1024; i++)
    {
        int idx = arrIdx[i];
        if (blockCnt[idx] > 0)
        {   
            auto block = Block::get(idx);
            ld << "blockid=" << std::dec << idx << ", tot=" << blockCnt[idx] << ", name='" << block->name << "', color=" << std::hex << block->color() << std::dec << std::endl;
        }
    }
    fd.close();
    ld.close();

    return 0;
}

    int32_t DimensionData_LevelDB::doOutput_Schematic(leveldb::DB* db)
    {
        for (const auto& schematic : listSchematic) {
            int32_t sizex = schematic->x2 - schematic->x1 + 1;
            int32_t sizey = schematic->y2 - schematic->y1 + 1;
            int32_t sizez = schematic->z2 - schematic->z1 + 1;

            // std::vector<int8_t>
            nbt::tag_byte_array blockArray;
            nbt::tag_byte_array blockDataArray;

            char keybuf[128];
            int32_t keybuflen;
            int32_t kw = dimId;
            //todohere todostopper - needs attention for 256h
            uint8_t kt = 0x30;
            leveldb::Status dstatus;

            log::info("  Processing Schematic: {}", schematic->toString());

            std::string svalue;
            const char* pchunk = nullptr;

            //int32_t color;
            //const char *pcolor = (const char*)&color;


            int32_t foundCt = 0, notFoundCt2 = 0;
            uint8_t blockid, blockdata;

            int32_t prevChunkX = 0;
            int32_t prevChunkZ = 0;
            bool prevChunkValid = false;

            // todozzz - if schematic area is larger than one chunk (65k byte array limit), then create multiple chunk-sized schematic files and name then .schematic.11.22 (where 11=x_chunk & 22=z_chunk)

            for (int32_t imageY = schematic->y1; imageY <= schematic->y2; imageY++) {

                for (int32_t imageZ = schematic->z1; imageZ <= schematic->z2; imageZ++) {
                    int32_t chunkZ = imageZ / 16;
                    int32_t coz = imageZ % 16;

                    for (int32_t imageX = schematic->x1; imageX <= schematic->x2; imageX++) {
                        int32_t chunkX = imageX / 16;
                        int32_t cox = imageX % 16;

                        if (prevChunkValid && (chunkX == prevChunkX) && (chunkZ == prevChunkZ)) {
                            // we already have the chunk
                        }
                        else {
                            // we need to read the chunk

                            prevChunkValid = false;

                            // construct key to get the chunk
                            if (dimId == kDimIdOverworld) {
                                //overworld
                                memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
                                memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
                                memcpy(&keybuf[8], &kt, sizeof(uint8_t));
                                keybuflen = 9;
                            }
                            else {
                                // nether (and probably any others that are added)
                                memcpy(&keybuf[0], &chunkX, sizeof(int32_t));
                                memcpy(&keybuf[4], &chunkZ, sizeof(int32_t));
                                memcpy(&keybuf[8], &kw, sizeof(int32_t));
                                memcpy(&keybuf[12], &kt, sizeof(uint8_t));
                                keybuflen = 13;
                            }

                            dstatus = db->Get(levelDbReadOptions, leveldb::Slice(keybuf, keybuflen), &svalue);
                            if (!dstatus.ok()) {
                                notFoundCt2++;
                                log::warn("Did not find chunk in leveldb x={} z={} status={}",
                                    chunkX, chunkZ, dstatus.ToString());
                                blockArray.push_back(0);
                                blockDataArray.push_back(0);
                                continue;
                            }

                            pchunk = svalue.data();

                            prevChunkValid = true;
                            prevChunkX = chunkX;
                            prevChunkZ = chunkZ;
                            foundCt++;
                        }

                        blockid = getBlockId_LevelDB_v2(pchunk, cox, coz, imageY);
                        blockdata = getBlockData_LevelDB_v2(pchunk, cox, coz, imageY);

                        blockArray.push_back(blockid);
                        blockDataArray.push_back(blockdata);
                    }
                }
            }

            std::string fnOut = (control.outputDir / ("bedrock_viz.schematic." + schematic->fn + ".nbt")).generic_string();

            writeSchematicFile(fnOut, sizex, sizey, sizez, blockArray, blockDataArray);

            //slogger.msg(kLogInfo1,"    Chunk Info: Found = %d / Not Found (our list) = %d / Not Found (leveldb) = %d\n", foundCt, notFoundCt1, notFoundCt2);
        }
        return 0;
    }

    int32_t DimensionData_LevelDB::doOutput(leveldb::DB* db, leveldb::DB* emptyWorld)
    {
        log::info("Do Output: {}", name);

        // we put images in subdir
        std::string fnBase = "bedrock_viz";
        std::string dirOut = (control.outputDir / "images").generic_string();
        local_mkdir(dirOut);

        log::info("  Generate Image");
        control.fnLayerTop[dimId] = std::string(dirOut + "/" + fnBase + "." + name + ".map.png");
        generateImage(control.fnLayerTop[dimId], kImageModeTerrain);

        if (dimId == control.blockListOutDim)
        {

            log::info("  Generate block list");
            generateBlockList(db, name, emptyWorld);
        }

        //doOutput_Schematic(db);

        // reset
        for(auto& i: Block::list()) {
            i->color_set_need_count = 0;
        }

        return 0;
    }


}