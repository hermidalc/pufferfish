#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib> // std::abs
#include <cmath> // std::abs
#include <memory>
#include <unordered_set>
#include "spdlog/spdlog.h"
#include "clipp.h"
#include "cedar.hpp"
#include "EquivalenceClassBuilder.hpp"
#include "CLI/Timer.hpp"
#include "PufferFS.hpp"
#include "cereal/archives/binary.hpp"
#include "cereal/types/vector.hpp"
#include "cereal/types/string.hpp"

#define LEFT true
#define RIGHT false
#define SCALE_FACTOR 1000000

struct CedarOpts {
    std::string taxonomyTree_filename;
    std::string refId2TaxId_filename;
    std::string mapperOutput_filename;
    std::string output_filename;
    std::string indexDir;
    std::string level = "species";
    bool flatAbund{false};
    double filterThreshold = 0;
    double eps = 0.001;
    size_t maxIter = 1000;
};

Cedar::Cedar(std::string& taxonomyTree_filename, 
                 std::string& refId2TaxId_filename, 
                 std::string pruneLevelIn,
                 double filteringThresholdIn,
                 std::string& indexDir,
                 bool flatAbundIn,
                 std::shared_ptr<spdlog::logger> loggerIn) {
    flatAbund = flatAbundIn;
    logger = loggerIn;
    logger->info("Cedar: Construct ..");

    /* {
      std::string rlPath = indexDir + "/reflengths.bin";
      if (puffer::fs::FileExists(rlPath.c_str())) {
        CLI::AutoTimer timer{"Loading reference lengths", CLI::Timer::Big};
        std::ifstream refLengthStream(rlPath);
        cereal::BinaryInputArchive refLengthArchive(refLengthStream);
        refLengthArchive(refLengths);
      } else {
        logger->error("Could not find reference genome lengths");
        std::exit(1);
      }
    } */
    // map rank string values to enum values
    filteringThreshold = filteringThresholdIn;
    if (!flatAbund) {
        pruningLevel = TaxaNode::str2rank(pruneLevelIn);
        std::ifstream tfile;
        uint32_t id, pid;
        std::string rank, name;
        // load the reference id (name) to its corresponding taxonomy id
        tfile.open(refId2TaxId_filename);
        while(!tfile.eof()) {
            tfile >> name >> id;
            refId2taxId[name] = id;
        }
        tfile.close();

        // load the taxonomy child-parent tree and the rank of each node
        tfile.open(taxonomyTree_filename);
        std::string tmp;
        while (!tfile.eof()) {
            tfile >> id >> tmp >> pid >> tmp >> rank >> tmp;
            size_t i = 0;
            while (i < tmp.size()-1 && isspace(tmp[i]))
                i++;
            if (tmp[i] != '|') {
                rank += " " + tmp;
            }
            taxaNodeMap[id] = TaxaNode(id, pid, TaxaNode::str2rank(rank));
            if (taxaNodeMap[id].isRoot()) {
                rootId = id;
                logger->info("Root Id : {}", id);
            }
            std::getline(tfile, tmp);
            
        }

        tfile.close(); 
    } else {
            /* {
      std::string rlPath = indexDir + "/ctable.bin";
      if (puffer::fs::FileExists(rlPath.c_str())) {
        CLI::AutoTimer timer{"Loading reference names", CLI::Timer::Big};
        std::ifstream refNameStream(rlPath);
        cereal::BinaryInputArchive refNameArchive(refNameStream);
        refNameArchive(refNames);
      } else {
        logger->error("Could not find reference genome names");
        std::exit(1);
      }
    } */


    }
}

bool Cedar::readHeader(std::ifstream& mfile) {
    std::string tmp, readType, refInfo;
    mfile >> tmp >> readType >> refInfo;
    if (tmp != "#")
        return false;
    if (readType == "LT:S") 
        isPaired = false;
    else if (readType == "LT:P")
        isPaired = true;
    else
        return false;

    if (refInfo.size() < 3 or refInfo.substr(0, 3) != "NT:")
        return false;
    
    size_t refCount = stoull(refInfo.substr(3));
    logger->info("Total # of References: {}", refCount);
    refLengths.reserve(refCount);
    refNames.reserve(refCount);
    size_t refLen;
    std::string refName;
    for (size_t i = 0; i < refCount; i++) {
        mfile >> refName >> refLen;
        refNames.push_back(refName);
        refLengths.push_back(refLen);
    }
    return true;
}

void Cedar::loadMappingInfo(std::string mapperOutput_filename) {
    int32_t rangeFactorization{4};
    std::string rid, tname, tmp;// read id, taxa name, temp
    uint64_t lcnt, rcnt, tid, puff_tid, ibeg, ilen;
    logger->info("Cedar: Load Mapping File ..");
    logger->info("Mapping Output File: {}", mapperOutput_filename);
    std::ifstream mfile(mapperOutput_filename);
    uint64_t rlen, mcnt; // taxa id, read mapping count, # of interals, interval start, interval length
    uint64_t totalReadCnt = 0, totalUnmappedReads = 0, seqNotFound = 0;
    if (!readHeader(mfile)) {
        logger->error("Invalid header for mapping output file.");
        std::exit(1);
    }
    logger->info("is dataset paired end? {}", isPaired);
    while (mfile >> rid >> mcnt) { 
        totalReadCnt++;
        if (totalReadCnt % 100000 == 0) {
            logger->info("Processed {} reads",totalReadCnt);
        }
        activeTaxa.clear();
        double readMappingsScoreSum = 0;
        std::vector<std::pair<uint64_t, double>> readPerStrainProbInst;
        readPerStrainProbInst.reserve(mcnt);

        if (mcnt != 0) {
            if (isPaired) {
                uint64_t rllen, rrlen;
                mfile >> rllen >> rrlen;
                rlen = rllen + rrlen;
            } else {
                mfile >> rlen;
            }

            std::set<uint64_t> seen;
            for (size_t mappingCntr = 0; mappingCntr < mcnt; mappingCntr++) {
                mfile >> puff_tid; // txp_id, txp_name, txp_len
                // first condition: Ignore those references that we don't have a
                // taxaId for secon condition: Ignore repeated exactly identical
                // mappings (FIXME thing)
                if (flatAbund or 
                    (refId2taxId.find(refNames[puff_tid]) != refId2taxId.end() and
                    activeTaxa.find(puff_tid) == activeTaxa.end())) {

                tid = flatAbund ? puff_tid : refId2taxId[refNames[puff_tid]];
                seqToTaxMap[puff_tid] = tid;
                activeTaxa.insert(puff_tid);

                // fetch the taxon from the map
                TaxaNode taxaPtr;
                mfile >> lcnt;
                if (isPaired)
                    mfile >> rcnt;
                for (size_t i = 0; i < lcnt; ++i) {
                    mfile >> ibeg >> ilen;
                    taxaPtr.addInterval(ibeg, ilen, LEFT);
                }
                if (isPaired)
                    for (size_t i = 0; i < rcnt; ++i) {
                    mfile >> ibeg >> ilen;
                    taxaPtr.addInterval(ibeg, ilen, RIGHT);
                    }
                taxaPtr.cleanIntervals(LEFT);
                taxaPtr.cleanIntervals(RIGHT);
                taxaPtr.updateScore();
                readPerStrainProbInst.emplace_back(puff_tid, static_cast<double>( taxaPtr.getScore()) / static_cast<double>(refLengths[puff_tid]) /* / static_cast<double>(tlen) */);
                readMappingsScoreSum += readPerStrainProbInst.back().second;
                } else { // otherwise we have to read till the end of the line and // throw it away
                std::getline(mfile, tmp);
                }
            } 
            if (activeTaxa.size() == 0) {
                seqNotFound++;
            } else {
                
                // it->first : strain id
                // it->second : prob of current read comming from this strain id
                for (auto it = readPerStrainProbInst.begin(); it != readPerStrainProbInst.end(); it++) {
                    it->second = it->second/readMappingsScoreSum; // normalize the probabilities for each read
                    // strain[it->first].first : read count for strainCnt
                    // strain[it->first].second : strain length
                    if (strain.find(it->first) == strain.end()) {
                        strain[it->first] = 1.0/static_cast<double>(readPerStrainProbInst.size());
                    }
                    else {
                        strain[it->first] += 1.0/static_cast<double>(readPerStrainProbInst.size());
                    }
                }
                // SAVE MEMORY, don't push this
                //readPerStrainProb.push_back(readPerStrainProbInst);

                // construct the range factorized eq class here 
                std::sort(readPerStrainProbInst.begin(), readPerStrainProbInst.end(), 
                [](std::pair<uint64_t, double>& a, std::pair<uint64_t, double>& b) {
                    return a.first < b.first;
                });
                std::vector<uint32_t> genomeIDs; genomeIDs.reserve(2*readPerStrainProbInst.size());
                std::vector<double> probs; probs.reserve(readPerStrainProbInst.size());
                for (auto it = readPerStrainProbInst.begin(); it != readPerStrainProbInst.end(); it++) {
                    genomeIDs.push_back(static_cast<uint32_t>(it->first));
                    probs.push_back(it->second);
                } 
                if (rangeFactorization > 0) {
                    int genomeSize = genomeIDs.size();
                    int rangeCount = std::sqrt(genomeSize) + rangeFactorization;
                    for (int i = 0; i < genomeSize; i++) {
                        int rangeNumber = probs[i] * rangeCount;
                        genomeIDs.push_back(static_cast<uint32_t>(rangeNumber));
                    }
                }
                readCnt++;
                TargetGroup tg(genomeIDs);
                eqb.addGroup(std::move(tg), probs);
            }
        } else {
            totalUnmappedReads++;
            std::getline(mfile, tmp);
        }
    }  
}

bool Cedar::basicEM(size_t maxIter, double eps) {
    eqb.finish();
    auto& eqvec = eqb.eqVec();
    int64_t maxSeqID{-1};
    for (auto& kv : strain) { 
        maxSeqID = (static_cast<int64_t>(kv.first) > maxSeqID) ? static_cast<int64_t>(kv.first) : maxSeqID;
    }

    std::vector<double> newStrainCnt(maxSeqID+1,0.0); 
    std::vector<double> strainCnt(maxSeqID+1);
    for (auto& kv : strain) { 
        strainCnt[kv.first] = kv.second;
    }

    logger->info("maxSeqID : {}", maxSeqID);
    logger->info("found : {} equivalence classes",eqvec.size()); 
    size_t totCount{0};
    for (auto& eqc : eqvec) {
        totCount += eqc.second.count;
    }
    logger->info("total staring count {}", totCount);
    logger->info("readCnt {}", readCnt);

    size_t cntr = 0;
    bool converged = false;
    while (cntr++ < maxIter && !converged) {
        // M step
        // Find the best (most likely) count assignment
        for (auto& eqc : eqvec) {
            auto& tg = eqc.first;
            auto& v = eqc.second;
            auto csize = v.weights.size();
            std::vector<double> tmpReadProb(csize, 0.0);
            double denom{0.0};
            for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                auto& tgt = tg.tgts[readMappingCntr];
               tmpReadProb[readMappingCntr] = v.weights[readMappingCntr] * strainCnt[tgt];// * (1.0/refLengths[tgt]);
               denom += tmpReadProb[readMappingCntr];
            }
            for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                auto& tgt = tg.tgts[readMappingCntr];
                newStrainCnt[tgt] += v.count * (tmpReadProb[readMappingCntr] / denom);
            }
        }
    
        // E step
        // normalize strain probabilities using the denum : p(s) = (count(s)/total_read_cnt) 
        double readCntValidator = 0;
        converged = true;   
        double maxDiff={0.0};
        for (size_t i = 0; i < strainCnt.size(); ++i) {
            readCntValidator += newStrainCnt[i];
            auto adiff = std::abs(newStrainCnt[i] - strainCnt[i]);
            if ( adiff > eps) {
                converged = false;
            }
            maxDiff = (adiff > maxDiff) ? adiff : maxDiff;
            strainCnt[i] = newStrainCnt[i];
            newStrainCnt[i] = 0.0;
        }

        if (std::abs(readCntValidator - readCnt) > 10) {
            logger->error("Total read count changed during the EM process");
            logger->error("original: {}, current : {}, diff : {}", readCnt, 
                           readCntValidator, std::abs(readCntValidator - readCnt));
            std::exit(1);
        }
        
        if (cntr > 0 and cntr % 100 == 0) {
            logger->info("max diff : {}", maxDiff);
        }
    }
    logger->info( "iterator cnt: {}", cntr); 

    // We have done the EM in the space of sequence / reference IDs
    // but we need to output results in terms of taxa IDs.  Here, we 
    // will map our reference IDs back to taxa IDs, and put the resulting
    // computed abundances in the "strain" member variable.
    decltype(strain) outputMap;
    outputMap.reserve(strain.size());
    for (auto& kv : strain) { 
        outputMap[seqToTaxMap[kv.first]] += strainCnt[kv.first];
    }
    std::swap(strain, outputMap);
    
    return cntr < maxIter;
}

void Cedar::serialize(std::string& output_filename) {
    logger->info("Write results in the file: {}", output_filename);
    std::ofstream ofile(output_filename);
    ofile << "taxaId\ttaxaRank\tcount\n";
    spp::sparse_hash_map<uint64_t, double> validTaxa;
    for (auto& kv : strain) {
        TaxaNode * walker = &taxaNodeMap[kv.first];
        while (!walker->isRoot() && walker->getRank() != pruningLevel) {
            walker = &taxaNodeMap[walker->getParentId()];
        }
        if (!walker->isRoot()) {
            if (validTaxa.find(walker->getId()) == validTaxa.end()) {
                validTaxa[walker->getId()] = kv.second;
            }
            else {
                validTaxa[walker->getId()] += kv.second;
            }
        }
    }
    for (auto& kv : validTaxa) { 
        ofile << kv.first << "\t" 
              << TaxaNode::rank2str(taxaNodeMap[kv.first].getRank()) 
              << "\t" << kv.second << "\n";
    }
    ofile.close();
}

void Cedar::serializeFlat(std::string& output_filename) {
    logger->info("Write results in the file: {}", output_filename);
    std::ofstream ofile(output_filename);
    ofile << "taxaId\ttaxaRank\tcount\n";
    for (auto& kv : strain) {
        ofile << refNames[kv.first] << "\t" 
              << "flat" 
              << "\t" << kv.second << "\n";
    }
    ofile.close();
}
/**
 * "How to run" example:
 * make Pufferfish!
 * In the Pufferfish build directory run the following command:
 * /usr/bin/time src/krakmap 
 * -t /mnt/scratch2/avi/meta-map/kraken/KrakenDB/taxonomy/nodes.dmp  
 * -s /mnt/scratch2/avi/meta-map/kraken/KrakenDB/seqid2taxid.map 
 * -m /mnt/scratch2/avi/meta-map/kraken/puff/dmps/HC1.dmp 
 * -o HC1.out 
 * -l genus (optional)
 * -f 0.8 (optional)
 **/
int main(int argc, char* argv[]) {
  (void)argc;
  using namespace clipp;
  CedarOpts kopts;
  bool showHelp{false};

  auto checkLevel = [](const char* lin) -> void {
    std::string l(lin);
    std::unordered_set<std::string> valid{"species", "genus", "family", "order", "class", "phylum"};
    if (valid.find(l) == valid.end()) {
      std::string s = "The level " + l + " is not valid.";
      throw std::range_error(s);
    }
  };

  auto cli = (
            (required("--flat").set(kopts.flatAbund, true) % "estimate flat abundance (i.e. there is no taxonomy given)"
            | (
              required("--taxtree", "-t") & value("taxtree", kopts.taxonomyTree_filename) % "path to the taxonomy tree file",
              required("--seq2taxa", "-s") & value("seq2taxa", kopts.refId2TaxId_filename) % "path to the refId 2 taxId file "
            )),
              required("--mapperout", "-m") & value("mapout", kopts.mapperOutput_filename) % "path to the pufferfish mapper output file",
              required("--output", "-o") & value("output", kopts.output_filename) % "path to the output file to write results",
              required("--index") & value("index", kopts.indexDir) % "pufferfish index directory",
              option("--maxIter", "-i") & value("iter", kopts.maxIter) % "maximum number of EM iteratons (default : 100)",
              option("--eps", "-e") & value("eps", kopts.eps) % "epsilon for EM convergence (default : 0.001)",
              option("--level", "-l") & value("level", kopts.level).call(checkLevel) % "choose between (species, genus, family, order, class, phylum). (default : species)",
              option("--filter", "-f") & value("filter", kopts.filterThreshold) % "choose the threshold [0,1] below which to filter out mappings (default : no filter)",
              option("--help", "-h").set(showHelp, true) % "show help",
              option("-v", "--version").call([]{std::cout << "version 0.1.0\n\n";}).doc("show version")
              );

  //Multithreaded console logger(with color support)
  auto console = spdlog::stderr_color_mt("console");

  decltype(parse(argc, argv, cli)) res;
  try {
    res = parse(argc, argv, cli);
    if (showHelp){
      std::cout << make_man_page(cli, "cedar"); 
      return 0;
    }
  } catch (std::exception& e) {
    std::cout << "\n\nparsing command line failed with exception: " << e.what() << "\n";
    std::cout << "\n\n";
    std::cout << make_man_page(cli, "cedar");
    return 1;
  }

  if(res) {
    Cedar cedar(kopts.taxonomyTree_filename, kopts.refId2TaxId_filename, kopts.level, kopts.filterThreshold, kopts.indexDir, kopts.flatAbund, console);
    cedar.loadMappingInfo(kopts.mapperOutput_filename);
    cedar.basicEM(kopts.maxIter, kopts.eps);
    if (!kopts.flatAbund) {
        cedar.serialize(kopts.output_filename);
    } else {
        cedar.serializeFlat(kopts.output_filename);
    }
    return 0;
  } else {
    std::cout << usage_lines(cli, "cedar") << '\n';
    return 1;
  }
  /*
  CLI::App app{"krakMap : Taxonomy identification based on the output of Pufferfish mapper through the same process as Kraken."};
  app.add_option("-t,--taxtree", kopts.taxonomyTree_filename,
                 "path to the taxonomy tree file")
      ->required();
  app.add_option("-s,--seq2taxa", kopts.refId2TaxId_filename, "path to the refId 2 taxaId file")
      ->required();
  app.add_option("-m,--mapperout", kopts.mapperOutput_filename, "path to the pufferfish mapper output file")
      ->required();
  app.add_option("-o,--output", kopts.output_filename, "path to the output file to write results")
      ->required();
  app.add_option("-i,--maxIter", kopts.maxIter, "maximum allowed number of iterations for EM")
      ->required(false);
  app.add_option("-e,--eps", kopts.eps, "epsilon for EM convergence condition")
      ->required(false);
  app.add_option("-l,--level", kopts.level, "choose between (species, genus, family, order, class, phylum). Default:species")
      ->required(false);
  app.add_option("-f,--filter", kopts.filterThreshold, "choose the threshold (0-1) to filter out mappings with a score below that. Default: no filter")
      ->required(false);
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }
  */
}