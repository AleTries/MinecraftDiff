#include "world/world.h"
#include "control.h"
#include "nbt.h"
#include "global.h"
#include "minecraft/conversion.h"
#include "utils/fs.h"
#include "asset.h"
#include "minecraft/v2/biome.h"
#include "minecraft/v2/block.h"

#include <ctime>
#include <leveldb/filter_policy.h>
#include <leveldb/cache.h>
#include <leveldb/env.h>


namespace
{
    // suggestion from mcpe_sample_setup.cpp
    class NullLogger : public leveldb::Logger {
    public:
        void Logv(const char*, va_list) override
        {
        }
    };

    // note: this is an attempt to remove "bad" chunks as seen in "nyan.zip" world
    bool legalChunkPos(int32_t chunkX, int32_t chunkZ)
    {
        if ((uint32_t)chunkX == 0x80000000 && (uint32_t)chunkZ == 0x80000000) {
            return false;
        }
        return true;
    }
}

namespace mcpe_viz
{
    MinecraftWorld_LevelDB::MinecraftWorld_LevelDB()
    {
        db = nullptr;

        levelDbReadOptions.fill_cache = false;
        // suggestion from leveldb/mcpe_sample_setup.cpp
        levelDbReadOptions.decompress_allocator = new leveldb::DecompressAllocator();


        dbOptions = std::make_unique<leveldb::Options>();
        //dbOptions->compressors[0] = new leveldb::ZlibCompressor();
        dbOptions->create_if_missing = false;

        // this filter is supposed to reduce disk reads - light testing indicates that it is faster when doing 'html-all'
        if (control.leveldbFilter > 0) {
            dbOptions->filter_policy = leveldb::NewBloomFilterPolicy(control.leveldbFilter);
        }

        dbOptions->block_size = control.leveldbBlockSize;

        //create a 40 mb cache (we use this on ~1gb devices)
        dbOptions->block_cache = leveldb::NewLRUCache(40 * 1024 * 1024);

        //create a 4mb write buffer, to improve compression and touch the disk less
        dbOptions->write_buffer_size = 4 * 1024 * 1024;
        dbOptions->info_log = new NullLogger();
        dbOptions->compression = leveldb::kZlibRawCompression;

        for (int32_t i = 0; i < kDimIdCount; i++) {
            dimDataList.push_back(std::make_unique<DimensionData_LevelDB>());
            dimDataList[i]->setDimId(i);
            dimDataList[i]->unsetChunkBoundsValid();
            dimDataList[i]->setName(kDimIdNames[i]);
        }
    }

    int32_t MinecraftWorld_LevelDB::parseLevelFile(const std::string& fname)
    {
        FILE* fp = fopen(fname.c_str(), "rb");
        if (!fp) {
            log::error("Failed to open input file (fn={} error={} ({}))", fname, strerror(errno), errno);
            return -1;
        }

        int32_t fVersion;
        int32_t bufLen;
        fread(&fVersion, sizeof(int32_t), 1, fp);
        fread(&bufLen, sizeof(int32_t), 1, fp);

        log::info("parseLevelFile: name={} version={} len={}", fname, fVersion, bufLen);

        int32_t ret = -2;
        if (bufLen > 0) {
            // read content
            char* buf = new char[bufLen];
            fread(buf, 1, bufLen, fp);
            fclose(fp);

            MyNbtTagList tagList;
            ret = parseNbt("level.dat: ", buf, bufLen, tagList);

            if (ret == 0) {
                nbt::tag_compound tc = tagList[0].second->as<nbt::tag_compound>();

                setWorldSpawnX(tc["SpawnX"].as<nbt::tag_int>().get());
                setWorldSpawnY(tc["SpawnY"].as<nbt::tag_int>().get());
                setWorldSpawnZ(tc["SpawnZ"].as<nbt::tag_int>().get());
                log::info("  Found World Spawn: x={} y={} z={}",
                    getWorldSpawnX(),
                    getWorldSpawnY(),
                    getWorldSpawnZ());

                setWorldSeed(tc["RandomSeed"].as<nbt::tag_long>().get());
            }

            delete[] buf;
        }
        else {
            fclose(fp);
        }

        return ret;
    }

    int32_t MinecraftWorld_LevelDB::init()
    {
        int32_t ret = parseLevelFile(std::string(control.dirLeveldb + "/level.dat"));
        if (ret != 0) {
            log::error("Failed to parse level.dat file.  Exiting...");
            log::error("** Hint: --db must point to the dir which contains level.dat");
            return -1;
        }

        ret = parseLevelName(std::string(control.dirLeveldb + "/levelname.txt"));
        if (ret != 0) {
            log::warn("WARNING: Failed to parse levelname.txt file.");
            log::warn("** Hint: --db must point to the dir which contains levelname.txt");
        }

        // update dimension data
        for (int32_t i = 0; i < kDimIdCount; i++) {
            dimDataList[i]->setWorldInfo(getWorldName(), getWorldSpawnX(), getWorldSpawnZ(), getWorldSeed());
        }

        return 0;
    }

    int32_t MinecraftWorld_LevelDB::dbOpen(const std::string& dirDb)
    {
        // todobig - leveldb read-only? snapshot?
        // note: seems impossible, see <https://github.com/google/leveldb/issues/182>
        log::info("DB Open: dir={}", dirDb);
        leveldb::Status openstatus = leveldb::DB::Open(*dbOptions, std::string(dirDb + "/db"), &db);
        log::info("DB Open Status: {} (block_size={} bloom_filter_bits={})", openstatus.ToString(), control.leveldbBlockSize, control.leveldbFilter);
        fflush(stderr);
        if (!openstatus.ok()) {
            log::error("LevelDB operation returned status={}", openstatus.ToString());
            
            if (control.tryDbRepair)
            {
                log::info("Attempting leveldb repair due to failed open");
                leveldb::Options options_;
                leveldb::Status repairstatus = leveldb::RepairDB(std::string(dirDb + "/db"), options_);

                if (repairstatus.ok())
                    log::info("LevelDB repair completed");
                else {
                    log::error("LevelDB repair failed");
                    exit(-2);
                }
            }

        }
        return 0;
    }


    int32_t MinecraftWorld_LevelDB::calcChunkBounds()
    {
        // see if we already calculated bounds
        bool passFlag = true;
        for (int32_t i = 0; i < kDimIdCount; i++) {
            if (!dimDataList[i]->getChunkBoundsValid()) {
                passFlag = false;
            }
        }
        if (passFlag) {
            return 0;
        }

        // clear bounds
        for (int32_t i = 0; i < kDimIdCount; i++) {
            dimDataList[i]->unsetChunkBoundsValid();
        }

        int32_t chunkX = -1, chunkZ = -1, chunkDimId = -1, chunkType = -1;

        log::info("Scan keys to get world boundaries");
        int32_t recordCt = 0;

        // todobig - is there a faster way to enumerate the keys?
        leveldb::Iterator* iter = db->NewIterator(levelDbReadOptions);
        leveldb::Slice skey;
        int32_t key_size;
        const char* key;
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
            skey = iter->key();
            key_size = int32_t(skey.size());
            key = skey.data();

            ++recordCt;
            if (control.shortRunFlag && recordCt > 1000) {
                break;
            }

            if (key_size == 9) {
                chunkX = myParseInt32(key, 0);
                chunkZ = myParseInt32(key, 4);
                chunkType = myParseInt8(key, 8);

                // sanity checks
                if (chunkType == 0x30) {
                    // pre-0.17 chunk block data
                    if (legalChunkPos(chunkX, chunkZ)) {
                        dimDataList[0]->addToChunkBounds(chunkX, chunkZ);
                    }
                }
            }
            if (key_size == 10) {
                // post-0.17 chunk block data
                chunkX = myParseInt32(key, 0);
                chunkZ = myParseInt32(key, 4);
                chunkType = myParseInt8(key, 8);

                // sanity checks
                if (chunkType == 0x2f) {
                    if (legalChunkPos(chunkX, chunkZ)) {
                        dimDataList[0]->addToChunkBounds(chunkX, chunkZ);
                    }
                }
            }
            else if (key_size == 13) {
                // pre-0.17 chunk block data
                chunkX = myParseInt32(key, 0);
                chunkZ = myParseInt32(key, 4);
                chunkDimId = myParseInt32(key, 8);
                chunkType = myParseInt8(key, 12);

                // sanity checks
                if (chunkType == 0x30) {
                    if (legalChunkPos(chunkX, chunkZ)) {
                        dimDataList[chunkDimId]->addToChunkBounds(chunkX, chunkZ);
                    }
                }
            }
            else if (key_size == 14) {
                // post-0.17 chunk block data
                chunkX = myParseInt32(key, 0);
                chunkZ = myParseInt32(key, 4);
                chunkDimId = myParseInt32(key, 8);
                chunkType = myParseInt8(key, 12);

                // sanity checks
                if (chunkType == 0x2f) {
                    if (legalChunkPos(chunkX, chunkZ)) {
                        dimDataList[chunkDimId]->addToChunkBounds(chunkX, chunkZ);
                    }
                }
            }
        }

        if (!iter->status().ok()) {
            log::warn("LevelDB operation returned status={}", iter->status().ToString());
        }
        delete iter;

        // mark bounds valid
        for (int32_t i = 0; i < kDimIdCount; i++) {
            dimDataList[i]->setChunkBoundsValid();
            dimDataList[i]->reportChunkBounds();
        }

        log::info("  {} records", recordCt);
        totalRecordCt = recordCt;

        return 0;
    }

    int32_t MinecraftWorld_LevelDB::dbParse()
    {
        char tmpstring[256];

        int32_t chunkX = -1, chunkZ = -1, chunkDimId = -1, chunkType = -1, chunkTypeSub = -1;
        int32_t chunkFormatVersion = 2; //todonow - get properly


        // we make sure that we know the chunk bounds before we start so that we can translate world coords to image coords
        calcChunkBounds();

        // report hide and force lists
        {
            log::info("Active 'hide-top', 'force-top', and 'geojson-block':");
            int32_t itemCt = 0;
            int32_t blockId;
            for (int32_t dimId = 0; dimId < kDimIdCount; dimId++) {
                dimDataList[dimId]->updateFastLists();
                for (const auto& iter : dimDataList[dimId]->blockHideList) {
                    blockId = iter;
                    log::info("  'hide-top' block: {} - {} (dimId={} blockId={} (0x{:x}))",
                        dimDataList[dimId]->getName(),
                        Block::queryName(blockId),
                        dimId, blockId, blockId);
                    itemCt++;
                }

                for (const auto& iter : dimDataList[dimId]->blockForceTopList) {
                    blockId = iter;
                    log::info("  'force-top' block: {} - {} (dimId={} blockId={} (0x{:x}))",
                        dimDataList[dimId]->getName(),
                              Block::queryName(blockId),
                              dimId, blockId, blockId);
                    itemCt++;
                }

                for (const auto& iter : dimDataList[dimId]->blockToGeoJSONList) {
                    blockId = iter;
                    log::info("  'geojson' block: {} - {} (dimId={} blockId={} (0x{:x}))",
                        dimDataList[dimId]->getName(),
                              Block::queryName(blockId),
                        dimId, blockId, blockId);
                    itemCt++;
                }
            }
            if (itemCt == 0) {
                log::info("  None");
            }
        }

        log::info("Parse all leveldb records");

        MyNbtTagList tagList;
        int32_t recordCt = 0, ret;

        leveldb::Slice skey, svalue;
        size_t key_size;
        size_t cdata_size;
        const char* key;
        const char* cdata;
        std::string dimName, chunkstr;

        leveldb::Iterator* iter = db->NewIterator(levelDbReadOptions);
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {

            // note: we get the raw buffer early to avoid overhead (maybe?)
            skey = iter->key();
            key_size = (int)skey.size();
            key = skey.data();

            svalue = iter->value();
            cdata_size = svalue.size();
            cdata = svalue.data();

            ++recordCt;
            if (control.shortRunFlag && recordCt > 1000) {
                break;
            }
            if ((recordCt % 10000) == 0) {
                double pct = (double)recordCt / (double)totalRecordCt;
                log::info("  Processing records: {} / {} ({:.1f}%)", recordCt, totalRecordCt, pct * 100.0);
            }

            // we look at the key to determine what we have, some records have text keys

            if (strncmp(key, "BiomeData", key_size) == 0) {
                // 0x61 +"BiomeData" -- snow accum? -- overworld only?
                log::trace("BiomeData value:");
                parseNbt("BiomeData: ", cdata, int32_t(cdata_size), tagList);
                // todo - parse tagList? snow accumulation amounts
            }
            else if (strncmp(key, "Overworld", key_size) == 0) {
                log::trace("Overworld value:");
                parseNbt("Overworld: ", cdata, int32_t(cdata_size), tagList);
                // todo - parse tagList? a list of "LimboEntities"
            }
            else if (strncmp(key, "~local_player", key_size) == 0) {
                log::trace("Local Player value:");
                ret = parseNbt("Local Player: ", cdata, int32_t(cdata_size), tagList);
                if (ret == 0) {
                    parseNbt_entity(-1, "", tagList, true, false, "Local Player", "");
                }
            }
            else if ((key_size >= 7) && (strncmp(key, "player_", 7) == 0)) {
                // note: key contains player id (e.g. "player_-1234")
                std::string playerRemoteId = std::string(&key[strlen("player_")], key_size - strlen("player_"));

                log::trace("Remote Player (id={}) value:", playerRemoteId);

                ret = parseNbt("Remote Player: ", cdata, int32_t(cdata_size), tagList);
                if (ret == 0) {
                    parseNbt_entity(-1, "", tagList, false, true, "Remote Player", playerRemoteId);
                }
            }
            else if (strncmp(key, "villages", key_size) == 0) {
                log::trace("Villages value:");
                parseNbt("villages: ", cdata, int32_t(cdata_size), tagList);
                // todo - parse tagList? usually empty, unless player is in range of village; test that!
            }
            else if (strncmp(key, "mVillages", key_size) == 0) {
                // todobig -- new for 0.13? what is it?
                log::trace("mVillages value:");
                ret = parseNbt("mVillages: ", cdata, int32_t(cdata_size), tagList);
                if (ret == 0) {
                    parseNbt_mVillages(tagList);
                }
            }
            else if (strncmp(key, "game_flatworldlayers", key_size) == 0) {
                // todobig -- what is it?
                // example data (standard flat): 5b 37 2c 33 2c 33 2c 32 5d
                log::trace("game_flatworldlayers value: (todo)");
                // parseNbt("game_flatworldlayers: ", cdata, cdata_size, tagList);
                // todo - parse tagList?
            }
            else if (strncmp(key, "idcounts", key_size) == 0) {
                // todobig -- new for 0.13? what is it? is it a 4-byte int?
                log::trace("idcounts value:");
                parseNbt("idcounts: ", cdata, int32_t(cdata_size), tagList);
            }
            else if (strncmp(key, "Nether", key_size) == 0) {
                log::trace("Nether value:");
                parseNbt("Nether: ", cdata, int32_t(cdata_size), tagList);
                // todo - parse tagList?  list of LimboEntities
            }
            else if (strncmp(key, "portals", key_size) == 0) {
                log::trace("portals value:");
                ret = parseNbt("portals: ", cdata, int32_t(cdata_size), tagList);
                if (ret == 0) {
                    parseNbt_portals(tagList);
                }
            }
            else if (strncmp(key, "AutonomousEntities", key_size) == 0) {
                log::trace("AutonomousEntities value:");
                ret = parseNbt("AutonomousEntities: ", cdata, int32_t(cdata_size), tagList);
                // todostopper - what to do with this info?
                //          if ( ret == 0 ) {
                //            parseNbt_portals(tagList);
                //          }
            }

            // todohere todonow -- new record like "dimension0" - presumably for other dims too
            //           looks like it could be partially text? nbt?
            /*
  WARNING: Unparsed Record:
  key_size=10
  key_string=[dimension0^AC<93><9A>]
  key_hex=[64 69 6d 65 6e 73 69 6f 6e 30]
  value_size=65
  value_hex=[0a 00 00 0a 09 00 6d 69 6e 65 73 68 61 66 74 00 0a 06 00 6f 63 65 61 6e 73 00 0a 09 00 73 63 61 74 74 65 72 65 64 00 0a 0a 00 73 74 72 6f 6e 67 68 6f 6c 64 00 0a 07 00 76 69 6c 6c 61 67 65 00 00]


  UNK: NBT Decode Start
  UNK: [] COMPOUND-1 {
  UNK:   [mineshaft] COMPOUND-2 {
  UNK:   } COMPOUND-2
  UNK:   [oceans] COMPOUND-3 {
  UNK:   } COMPOUND-3
  UNK:   [scattered] COMPOUND-4 {
  UNK:   } COMPOUND-4
  UNK:   [stronghold] COMPOUND-5 {
  UNK:   } COMPOUND-5
  UNK:   [village] COMPOUND-6 {
  UNK:   } COMPOUND-6
  UNK: } COMPOUND-1
  UNK: NBT Decode End (1 tags)

*/
            else if (strncmp(key, "dimension", 9) == 0) {
                std::string keyString(key, key_size);
                log::debug("Dimension chunk -- key: ({}) value:", keyString);
                ret = parseNbt("Dimension: ", cdata, int32_t(cdata_size), tagList);
                // todostopper - what to do with this info?
                //          if ( ret == 0 ) {
                //            parseNbt_portals(tagList);
                //          }
            }
            else if (key_size == 9 || key_size == 10 || key_size == 13 || key_size == 14) {

                // these are probably chunk records, we parse the key and determine what we've got

                chunkTypeSub = 0;

                if (key_size == 9) {
                    // overworld chunk
                    chunkX = myParseInt32(key, 0);
                    chunkZ = myParseInt32(key, 4);
                    chunkDimId = kDimIdOverworld;
                    chunkType = myParseInt8(key, 8);
                    dimName = "overworld";
                    chunkFormatVersion = 2; //todonow - get properly
                }
                else if (key_size == 10) {
                    // overworld chunk
                    chunkX = myParseInt32(key, 0);
                    chunkZ = myParseInt32(key, 4);
                    chunkDimId = kDimIdOverworld;
                    chunkType = myParseInt8(key, 8);
                    chunkTypeSub = myParseInt8(key, 9); // todonow - rename
                    dimName = "overworld";
                    chunkFormatVersion = 3; //todonow - get properly
                }
                else if (key_size == 13) {
                    // non-overworld chunk
                    chunkX = myParseInt32(key, 0);
                    chunkZ = myParseInt32(key, 4);
                    chunkDimId = myParseInt32(key, 8);
                    chunkType = myParseInt8(key, 12);
                    dimName = "nether";
                    chunkFormatVersion = 2; //todonow - get properly

                    // adjust weird dim id's
                    if (chunkDimId == 0x32373639) {
                        chunkDimId = kDimIdTheEnd;
                    }
                    if (chunkDimId == 0x33373639) {
                        chunkDimId = kDimIdNether;
                    }

                    // check for new dim id's
                    if (chunkDimId != kDimIdNether && chunkDimId != kDimIdTheEnd) {
                        log::warn("UNKNOWN -- Found new chunkDimId=0x{:x} -- we are not prepared for that -- skipping chunk", chunkDimId);
                        continue;
                    }
                }
                else if (key_size == 14) {
                    // non-overworld chunk
                    chunkX = myParseInt32(key, 0);
                    chunkZ = myParseInt32(key, 4);
                    chunkDimId = myParseInt32(key, 8);
                    chunkType = myParseInt8(key, 12);
                    chunkTypeSub = myParseInt8(key, 13); // todonow - rename
                    dimName = "nether";
                    chunkFormatVersion = 3; //todonow - get properly

                    // adjust weird dim id's
                    if (chunkDimId == 0x32373639) {
                        chunkDimId = kDimIdTheEnd;
                    }
                    if (chunkDimId == 0x33373639) {
                        chunkDimId = kDimIdNether;
                    }

                    // check for new dim id's
                    if (chunkDimId != kDimIdNether && chunkDimId != kDimIdTheEnd) {
                        log::warn("UNKNOWN -- Found new chunkDimId=0x{:x} -- we are not prepared for that -- skipping chunk", chunkDimId);
                        continue;
                    }
                }

                // we check for corrupt chunks
                if (!legalChunkPos(chunkX, chunkZ)) {
                    log::warn("Found a chunk with invalid chunk coordinates cx={} cz={}", chunkX, chunkZ);
                    continue;
                }

                // report info about the chunk
                chunkstr = dimName + "-chunk: ";
                sprintf(tmpstring, "%d %d (type=0x%02x) (subtype=0x%02x) (size=%d)", chunkX, chunkZ, chunkType,
                    chunkTypeSub, (int32_t)cdata_size);
                chunkstr += tmpstring;
                if (true) {
                    // show approximate image coordinates for chunk
                    double tix, tiy;
                    dimDataList[chunkDimId]->worldPointToImagePoint(chunkX * 16, chunkZ * 16, tix, tiy, false);
                    int32_t imageX = int32_t(tix);
                    int32_t imageZ = int32_t(tiy);
                    sprintf(tmpstring, " (image %d %d)", (int32_t)imageX, (int32_t)imageZ);
                    chunkstr += tmpstring;
                }
                log::trace("{}", chunkstr);

                // see what kind of chunk we have
                // tommo posted useful info about the various record types here (around 0.17 beta):
                //   https://www.reddit.com/r/MCPE/comments/5cw2tm/level_format_changes_in_mcpe_0171_100/
                switch (chunkType) {
                case 0x30:
                    // "LegacyTerrain"
                    // chunk block data
                    // we do the parsing in the destination object to save memcpy's
                    // todonow - would be better to get the version # from the proper chunk record (0x76)
                    dimDataList[chunkDimId]->addChunk(2, chunkX, 0, chunkZ, cdata, cdata_size);
                    break;

                case 0x31:
                    // "BlockEntity"
                    // tile entity record (e.g. a chest)
                    log::debug("{} 0x31 chunk (tile entity data):", dimName);
                    ret = parseNbt("0x31-te: ", cdata, int32_t(cdata_size), tagList);
                    if (ret == 0) {
                        parseNbt_tileEntity(chunkDimId, dimName + "-", tagList);
                    }
                    break;

                case 0x32:
                    // "Entity"
                    // entity record (e.g. a mob)
                    log::debug("{} 0x32 chunk (entity data):", dimName);
                    ret = parseNbt("0x32-e: ", cdata, int32_t(cdata_size), tagList);
                    if (ret == 0) {
                        parseNbt_entity(chunkDimId, dimName + "-", tagList, false, false, "", "");
                    }
                    break;

                case 0x33:
                    // "PendingTicks"
                    // todo - this appears to be info on blocks that can move: water + lava + fire + sand + gravel
                    // todo - new nether has slowed things down quite a bit
                    log::trace("{} 0x33 chunk (tick-list):", dimName);
                    //parseNbt("0x33-tick: ", cdata, int32_t(cdata_size), tagList);
                    // todo - parse tagList?
                    // todobig - could show location of active fires
                    break;

                case 0x34:
                    // "BlockExtraData"
                    log::debug("{} 0x34 chunk (TODO - MYSTERY RECORD - BlockExtraData)",
                        dimName.c_str());
                    if (control.verboseFlag) {
                        printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), false);
                    }
                    // according to tommo (https://www.reddit.com/r/MCPE/comments/5cw2tm/level_format_changes_in_mcpe_0171_100/)
                    // "BlockExtraData"
                    /*
                       0x34 ?? does not appear to be NBT data -- overworld only? -- perhaps: b0..3 (count); for each: (int32_t) (int16_t)
                       -- there are 206 of these in "another1" world
                       -- something to do with snow?
                       -- to examine data:
                       cat (logfile) | grep "WARNING: Unknown key size" | grep " 34\]" | cut -b75- | sort | nl
                    */
                    break;

                case 0x35:
                    // "BiomeState"
                    log::debug("{} 0x35 chunk (BiomeState)",
                        dimName);
                    if (control.verboseFlag) {
                        printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), false);
                    }
                    // according to tommo (https://www.reddit.com/r/MCPE/comments/5cw2tm/level_format_changes_in_mcpe_0171_100/)
                    // "BiomeState"
                    /*
                      0x35 ?? -- both dimensions -- length 3,5,7,9,11 -- appears to be: b0 (count of items) b1..bn (2-byte ints)
                      -- there are 2907 in "another1"
                      -- to examine data:
                      cat (logfile) | grep "WARNING: Unknown key size" | grep " 35\]" | cut -b75- | sort | nl
                    */
                    break;

                case 0x36:
                    // new for v1.2?
                    log::trace("{} 0x36 chunk (FinalizedState)", dimName);
                    if (control.verboseFlag) {
                        printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), false);
                    }
                    // todo - what is this?
                    // appears to be a single 4-byte integer?
                    break;

                case 0x39:
                    // Bounding boxes for structure spawns stored in binary format
                    log::debug("{} 0x39 chunk (HardCodedSpawnAreas)", dimName);
                    if (control.verboseFlag) {
                        printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), false);
                    }
                    // todo - probably used for outposts and things of that nature
                    break;

                case 0x3b:
                    // Appears to be a list of checksums for chunk data. Upcoming in 1.16
                    log::trace("{} 0x3b chunk (checksum?)", dimName);
                    if (control.verboseFlag) {
                        printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), false);
                    }
                    // todo - what is this?
                    break;

                case 0x76:
                    // "Version"
                {
                    // this record is not very interesting, we usually hide it
                    // note: it would be interesting if this is not == 2 (as of MCPE 0.12.x it is always 2)
                    
                    // chunk versions have changed many times since this was originally included. 
                    // it seems unnecessary to keep track of this for anything other than trace information
                    log::trace("{} 0x76 chunk (world format version): v={}"
                        ,dimName
                        ,int(cdata[0]));
                    /*
                    if (control.verboseFlag || ((cdata[0] != 2) && (cdata[0] != 3) && (cdata[0] != 9))) {
                        if (cdata[0] != 2 && cdata[0] != 9) {
                            log::debug("UNKNOWN CHUNK VERSION!  {} 0x76 chunk (world format version): v={}",
                                dimName, int(cdata[0]));
                        }
                        else {
                            log::debug("{} 0x76 chunk (world format version): v={}",
                                dimName, int(cdata[0]));
                        }
                    }
                    */
                }
                break;

                case 0x2f:
                    // "SubchunkPrefix"
                    // chunk block data - 10241 bytes
                    // todonow -- but have also seen 6145 on v1.1?
                    // we do the parsing in the destination object to save memcpy's
                    // todonow - would be better to get the version # from the proper chunk record (0x76)
                {
                    int32_t chunkY = chunkTypeSub;
                    // check the first byte to see if anything interesting is in it
                    if (cdata[0] != 0) {
                        //logger.msg(kLogInfo1, "WARNING: UNKNOWN Byte 0 of 0x2f chunk: b0=[%d 0x%02x]\n", (int)cdata[0], (int)cdata[0]);
                        dimDataList[chunkDimId]->addChunk(7, chunkX, chunkY, chunkZ, cdata, cdata_size);
                    }
                    else {
                        if (cdata_size != 6145 && cdata_size != 10241) {
                            log::warn("UNKNOWN cdata_size={} of 0x2f chunk", cdata_size);
                        }
                        dimDataList[chunkDimId]->addChunk(chunkFormatVersion, chunkX, chunkY, chunkZ, cdata,
                            cdata_size);
                    }
                }
                break;

                case 0x2d:
                    // "Data2D"
                    // chunk column data - 768 bytes
                    // format appears to be:
                    // 16x16 of 2-byte ints for HEIGHT OF TOP BLOCK
                    // 8x8 of 4-byte ints for BIOME and GRASS COLOR
                    // todonow todobig todohere -- this appears to be an MCPE bug, it should be 16x16, right?
                    // also - grass colors are pretty weird (some are 01 01 01)

                    // todonow - would be better to get the version # from the proper chunk record (0x76)
                {
                    dimDataList[chunkDimId]->addChunkColumnData(3, chunkX, chunkZ, cdata, int32_t(cdata_size));
                }
                break;


                /*
   todohere todonow
   new chunk types in 0.17
   0x2d] - size=768
   0x2f 0x00] - size 10241
   ...
   0x2f 0x07] - size 10241


   per chunk data: 2.5 bytes / block
   block id = 1 byte
   block data = 4-bits
   skylight = 4-bits
   blocklight = 4-bits

   16x16x16 of this = 10,240!!
   what is the one extra byte... hmmmm

   NOTE! as of at least v1.1.0 there are also records that are 6145 bytes - they appear
   to exclude the block/sky light parts


   per column data: 5-bytes per column
   height of top block = 1 byte
   grass-and-biome = 4-bytes = lsb bome, high 3-bytes are RGB grass color

   0x2d chunks are 768 bytes which could be column data
   16 x 16 x 3 = 768
   so 3 bytes per column = grass/biome + height + top block
   could this be grass color only?


   0x2f N] chunks are 10241
   this could be 16x16 for 16 vertical blocks
   16 of these would cover 256 build height

   if blocks are 8-bits, that would be 8,192 of the size
   which leaves 2049
   we'd still need block data which is 4-bits per block
   which is: 4,096 bytes.... what's going on here?!
*/

                default:
                    log::debug("{} unknown chunk - key_size={} type=0x{:x} length={}", dimName, key_size, chunkType, cdata_size);
                    printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), true);
                    break;
                }
            }
            else {
                log::debug("Unknown chunk - key_size={} cdata_size={}", key_size, cdata_size);
                printKeyValue(key, int32_t(key_size), cdata, int32_t(cdata_size), true);
            }
        }
        log::info("Read {} records", recordCt);
        log::info("Status: {}", iter->status().ToString());

        if (!iter->status().ok()) {
            log::warn("LevelDB operation returned status={}", iter->status().ToString());
        }
        delete iter;

        return 0;
    }

    int32_t MinecraftWorld_LevelDB::doOutput()
    {
        calcChunkBounds();

        // todonow todobig todostopper -- how to handle worlds that are larger than png dimensional limits (see midgard world file)
        leveldb::DB *emptyWorld = nullptr;
        if (control.emptyDbName != "<none>")
        {
            leveldb::Status openstatus = leveldb::DB::Open(*dbOptions, std::string(control.emptyDbName + "/db"), &emptyWorld);
            log::info("DB Open Status: {} (block_size={} bloom_filter_bits={})", openstatus.ToString(), control.leveldbBlockSize, control.leveldbFilter);
            fflush(stderr);
            if (!openstatus.ok()) {
               return -1;
            }
        }

        for (int32_t i = 0; i < kDimIdCount; i++) {
            dimDataList[i]->doOutput(db, emptyWorld);
        }

        return 0;
    }

    std::unique_ptr<MinecraftWorld_LevelDB> world;
}
