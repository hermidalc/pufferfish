//
// Created by Fatemeh Almodaresi on 8/28/18.
//

#ifndef PUFFERFISH_CHAINFINDER_H
#define PUFFERFISH_CHAINFINDER_H

#include "CommonTypes.hpp"

class MemClusterer {
public:
    static inline float fastlog2(float x) {
        union {
            float f;
            uint32_t i;
        } vx = {x};
        union {
            uint32_t i;
            float f;
        } mx = {(vx.i & 0x007FFFFF) | 0x3f000000};
        float y = vx.i;
        y *= 1.1920928955078125e-7f;

        return y - 124.22551499f
               - 1.498030302f * mx.f
               - 1.72587999f / (0.3520887068f + mx.f);
    }

    static inline float fasterlog2(float x) {
        union {
            float f;
            uint32_t i;
        } vx = {x};
        float y = vx.i;
        y *= 1.1920928955078125e-7f;
        return y - 126.94269504f;
    }

    bool clusterMems(std::vector<std::pair<int, util::ProjectedHits>> &hits,
                     spp::sparse_hash_map<pufferfish::common_types::ReferenceID, std::vector<util::MemCluster>> &memClusters,
                     uint32_t maxSpliceGap, std::vector<util::UniMemInfo> &memCollection, bool verbose = false) {
        if (verbose)
            std::cerr << "\n[CLUSTERMEMS]\n";

        using namespace pufferfish::common_types;
        //(void)verbose;

        if (hits.empty()) {
            return false;
        }

        // Map from (reference id, orientation) pair to a cluster of MEMs.
        std::map<std::pair<ReferenceID, bool>, std::vector<util::MemInfo>>
                trMemMap;
        // here we guarantee that even if later we fill up
        // every gap between the hits and before the first and after the last hit
        // still the memCollection size doesn't change and hence the pointers are valid
        memCollection.reserve(200 * 2 * hits.size() + 1);
        if (verbose)
            std::cerr << "\nreserved memCollection size: " << 200 * 2 * hits.size() + 1 << "\n";
        for (auto &hit : core::range<decltype(hits.begin())>(hits.begin(), hits.end())) {
            auto &readPos = hit.first;
            auto &projHits = hit.second;
            // NOTE: here we rely on internal members of the ProjectedHit (i.e., member variables ending in "_").
            // Maybe we want to change the interface (make these members public or provide accessors)?
            auto &refs = projHits.refRange;
            if (verbose)
                std::cerr << "total number of references found: " << refs.size() << "\n";
            if (refs.size() < 200) {
                memCollection.emplace_back(projHits.contigIdx_, projHits.contigOrientation_,
                                           readPos, projHits.k_, projHits.contigPos_,
                                           projHits.globalPos_ - projHits.contigPos_, projHits.contigLen_);
                auto memItr = std::prev(memCollection.end());
                for (auto &posIt : refs) {
                    auto refPosOri = projHits.decodeHit(posIt);
                    trMemMap[std::make_pair(posIt.transcript_id(), refPosOri.isFW)]
                            .emplace_back(memItr, refPosOri.pos);
                }

                if (verbose)
                    std::cerr << "total number of mappings found: " << refs.size() << "\n";

            }
        }

        for (auto &trMem : core::range<decltype(trMemMap.begin())>(trMemMap.begin(), trMemMap.end())) {
            auto &trOri = trMem.first;
            auto &tid = trOri.first;
            auto &isFw = trOri.second;
            auto &memList = trMem.second;
            // sort memList according to mem reference positions
            if (isFw)
                std::sort(memList.begin(), memList.end(),
                          [](util::MemInfo &q1, util::MemInfo &q2) -> bool {
                              return q1.tpos == q2.tpos ? q1.memInfo->rpos < q2.memInfo->rpos : q1.tpos <
                                                                                                q2.tpos; // sort based on tpos
                          });
            else
                std::sort(memList.begin(), memList.end(),
                          [](util::MemInfo &q1, util::MemInfo &q2) -> bool {
                              return q1.tpos == q2.tpos ? q1.memInfo->rpos > q2.memInfo->rpos : q1.tpos <
                                                                                                q2.tpos; // sort based on tpos
                          });

            std::vector<util::MemCluster> currMemClusters;
            // cluster MEMs so that all the MEMs in one cluster are concordant.
            for (auto &hit : core::range<decltype(memList.begin())>(memList.begin(), memList.end())) {
                //bool foundAtLeastOneCluster = false;
                //bool gapIsSmall = false;
                bool addNewCluster = currMemClusters.size() == 0;
                bool foundAtLeastOne = false;
                //bool foundPrev = false;
                for (auto prevClus = currMemClusters.rbegin();
                     prevClus != currMemClusters.rend(); prevClus++) {
                    if (hit.tpos - prevClus->getTrLastHitPos() < maxSpliceGap) {
                        // if the distance between last mem and the new one is NOT longer than maxSpliceGap
                        //gapIsSmall = true;
                        if (
                                (hit.tpos >= (prevClus->getTrLastHitPos() + prevClus->getTrLastMemLen()) &&
                                 (
                                         (isFw && hit.memInfo->rpos >=
                                                  (prevClus->getReadLastHitPos() + prevClus->getTrLastMemLen())) ||
                                         (!isFw &&
                                          (hit.memInfo->rpos + hit.memInfo->memlen) <= prevClus->getReadLastHitPos())
                                 )) ||
                                // then hit.tpos < (prevClus->getTrLastHitPos() + prevClus->getTrLastMemLen() and
                                //(hit.tpos < (prevClus->getTrLastHitPos() + prevClus->getTrLastMemLen())) and
                                (isFw && (prevClus->getTrLastHitPos() + prevClus->getTrLastMemLen() - hit.tpos) ==
                                         (prevClus->getReadLastHitPos() + prevClus->getTrLastMemLen() -
                                          hit.memInfo->rpos)) ||
                                (!isFw && (prevClus->getTrLastHitPos() + prevClus->getTrLastMemLen() - hit.tpos) ==
                                          (hit.memInfo->rpos + hit.memInfo->memlen - prevClus->getReadLastHitPos()))
                                ) {
                            // NOTE: Adds a new mem to the list of cluster mems and updates the coverage
                            prevClus->addMem(hit.memInfo, hit.tpos);
                            //if(verbose){std::cerr << "len after putting in cluster: "<<hit.memInfo->memlen<<"\n";}
                            foundAtLeastOne = true;
                            //foundPrev = true;
                            //addNewCluster = false;
                            //break;
                        }
                    } else {
                        if (!foundAtLeastOne)
                            addNewCluster = true;
                        break;
                    }
                }

                if (addNewCluster) {
                    //auto prevLastIndex = static_cast<int32_t>(currMemClusters.size()) - 1;
                    // Create the new clusters on the end of the currMemClusters vector
                    currMemClusters.emplace_back(isFw);
                    auto &newClus = currMemClusters.back();
                    /*if ((prevLastIndex > 0) and gapIsSmall) {
                      auto& lastClus = currMemClusters[prevLastIndex];
                      // add all previous compatable mems before this last one that was crossed
                      for (auto mem = lastClus.mems.begin(); mem != lastClus.mems.end() && mem->memInfo->rpos < hit.memInfo->rpos; mem++) {
                        newClus.mems.emplace_back(mem->memInfo, mem->tpos);
                      }
                      }*/
                    // NOTE: Adds a new mem to the list of cluster mems and updates the coverage
                    newClus.addMem(hit.memInfo, hit.tpos);
                    //if(verbose){std::cerr << "len after putting in cluster: "<<hit.memInfo->memlen<<"\n";}
                }
            }
            /*for (auto& clus : currMemClusters) {
              if (clus.mems.size() > 69) {
                std::cerr << "mem size: " << clus.mems.size() << "\n";
                for (auto& mem : clus.mems) {
                  std::cerr << "t" << mem.tpos << " r" << mem.memInfo->rpos << " cid" << mem.memInfo->cid << " -- ";
                }
                std::cerr << "\n";
              }
              }*/
            if (verbose) {
                std::cerr << "\ntid" << tid << " " << tid << /*pfi_->refName(tid) << */" , isFw:" << isFw
                          << " cluster size:" << currMemClusters.size() << "\n";
                for (auto &clus : currMemClusters) {
                    std::cerr << "mem size: " << clus.mems.size() << "\n";
                    for (auto &mem : clus.mems) {
                        std::cerr << "t" << mem.tpos << " r" << mem.memInfo->rpos << " cid" << mem.memInfo->cid
                                  << " -- ";
                    }
                    std::cerr << "\n";
                }
            }
            // This is kind of inefficient (copying the currMemClusters while probably we can build it on the fly)
            memClusters[tid].insert(memClusters[tid].end(), std::make_move_iterator(currMemClusters.begin()),
                                    std::make_move_iterator(currMemClusters.end()));
        }
        if (verbose)
            std::cerr << "\n[END OF CLUSTERMEMS]\n";

        return true;
    }

    bool fillMemCollection(std::vector<std::pair<int, util::ProjectedHits>> &hits,
                           std::map<std::pair<pufferfish::common_types::ReferenceID, bool>, std::vector<util::MemInfo>> &trMemMap,
                           std::vector<util::UniMemInfo> &memCollection, util::ReadEnd re, bool verbose = false) {
        if (verbose)
            std::cerr << "\n[FIND_OPT_CHAIN]\n";

        using namespace pufferfish::common_types;
        //(void)verbose;

        if (hits.empty()) {
            return false;
        }

        // here we guarantee that even if later we fill up
        // every gap between the hits and before the first and after the last hit
        // still the memCollection size doesn't change and hence the pointers are valid
        memCollection.reserve(200 * 2 * hits.size() + 1);
        if (verbose)
            std::cerr << "\nreserved memCollection size: " << 200 * 2 * hits.size() + 1 << "\n";
        for (auto &hit : core::range<decltype(hits.begin())>(hits.begin(), hits.end())) {
            auto &readPos = hit.first;
            auto &projHits = hit.second;
            // NOTE: here we rely on internal members of the ProjectedHit (i.e., member variables ending in "_").
            // Maybe we want to change the interface (make these members public or provide accessors)?
            auto &refs = projHits.refRange;
            if (verbose)
                std::cerr << "total number of references found: " << refs.size() << "\n";
            if (refs.size() < 200) {
                memCollection.emplace_back(projHits.contigIdx_, projHits.contigOrientation_,
                                           readPos, projHits.k_, projHits.contigPos_,
                                           projHits.globalPos_ - projHits.contigPos_, projHits.contigLen_, re);
                auto memItr = std::prev(memCollection.end());
                for (auto &posIt : refs) {
                    auto refPosOri = projHits.decodeHit(posIt);
                    trMemMap[std::make_pair(posIt.transcript_id(), true)]
                            .emplace_back(memItr, refPosOri.pos, refPosOri.isFW);
                }

                if (verbose)
                    std::cerr << "total number of mappings found: " << refs.size() << "\n";

            }
        }
        return true;
    }


    // We have both left and right end mems
    bool
    findOptChain(std::map<std::pair<pufferfish::common_types::ReferenceID, bool>, std::vector<util::MemInfo>> &trMemMap,
                 std::vector<util::JointMems> &jointMemsList,
                 std::vector<util::MemCluster> &all,
                 uint32_t maxSpliceGap,
                 uint32_t maxFragmentLength,
                 bool verbose = false) {
        if (verbose)
            std::cerr << "\n[START OF FIND_OPT_CHAIN]\n";

        using namespace pufferfish::common_types;

        double globalScore{-std::numeric_limits<double>::infinity()};
        std::vector<double> f;
        std::vector<int32_t> p;
        all.reserve(200 * 2 * trMemMap.size());
        all.push_back(util::MemCluster(1));
        bool no = trMemMap.size() == 0;
        for (auto &trMem : core::range<decltype(trMemMap.begin())>(trMemMap.begin(), trMemMap.end())) {
            auto &trOri = trMem.first;
            auto &tid = trOri.first;
            auto &isFw = trOri.second;
            auto &memList = trMem.second;
            // sort memList according to mem reference positions
            std::sort(memList.begin(), memList.end(),
                      [](util::MemInfo &q1, util::MemInfo &q2) -> bool {
                          auto q1ref = q1.tpos + q1.memInfo->memlen;
                          auto q2ref = q2.tpos + q2.memInfo->memlen;
                          auto q1read = q1.memInfo->rpos + q1.memInfo->memlen;
                          auto q2read = q2.memInfo->rpos + q2.memInfo->memlen;
                          return q1ref != q2ref ? q1ref < q2ref :
                                 (q1.isFw && q2.isFw ? q1read < q2read : q1read > q2read);// sort based on tpos
                      });
            if (verbose) {
                std::cerr << "\ntid" << tid << /*pfi_->refName(tid) << */" , isFw:" << isFw << "\n";
                for (auto &m : memList) {
                    std::cerr << "\ttpos:" << m.tpos << " rpos:" << m.memInfo->rpos << " len:" << m.memInfo->memlen
                              << "\n";
                }
            }

            //auto minPosIt = memList.begin();
            // find the valid chains
            // Use variant of minimap2 scoring (Li 2018)
            // https://academic.oup.com/bioinformatics/advance-article/doi/10.1093/bioinformatics/bty191/4994778
            auto alpha = [](uint32_t qdiff, uint32_t rdiff, uint32_t ilen,
                            bool readsAreDifferent) -> double {
                double mindiff = (qdiff < rdiff) ? qdiff : rdiff;
                double score = ilen;
                if (readsAreDifferent)
                    mindiff = rdiff;
                return (score < mindiff) ? score : mindiff;
            };

            auto beta = [maxSpliceGap](int32_t qdiff, int32_t rdiff, double avgseed,
                                       bool readsAreDifferent,
                                       bool sameOrientation, uint32_t memLen) -> double {
                if (readsAreDifferent) {
                    double penalty = sameOrientation ? 5 : 1;
                    int32_t al = std::abs(rdiff - memLen);
                    return penalty *
                           std::min(0.01 * avgseed * al, static_cast<double>(fastlog2(static_cast<float>(al))));
                }
                if (!sameOrientation or (qdiff < 0 or ((uint32_t) std::max(qdiff, rdiff) > maxSpliceGap))) {
                    return std::numeric_limits<double>::infinity();
                }
                double l = qdiff - rdiff;
                int32_t al = std::abs(l);
                return (l == 0) ? 0.0 : (0.01 * avgseed * al + 0.5 * fastlog2(static_cast<float>(al)));
            };

            double bottomScore = std::numeric_limits<double>::lowest();
            double bestScore = bottomScore;
            //int32_t bestChainEnd = -1;
            std::vector<int32_t> bestChainEndList;
            double avgseed = 31.0;
            f.clear();
            p.clear();
            //auto lastHitId = static_cast<int32_t>(memList.size() - 1);
            for (int32_t i = 0; i < static_cast<int32_t>(memList.size()); ++i) {
                auto &hi = memList[i];

                auto qposi = hi.memInfo->rpos + hi.memInfo->memlen;
                auto rposi = hi.tpos + hi.memInfo->memlen;

                double baseScore = static_cast<double>(hi.memInfo->memlen);
                p.push_back(i);
                f.push_back(baseScore);

                // possible predecessors in the chain
                int32_t numRounds{2};
                (void) numRounds;
                for (int32_t j = i - 1; j >= 0; --j) {
                    auto &hj = memList[j];

                    auto qposj = hj.memInfo->rpos + hj.memInfo->memlen;
                    auto rposj = hj.tpos + hj.memInfo->memlen;

                    auto qdiff = hi.isFw && hj.isFw ? qposi - qposj :
                                 (qposj - hj.memInfo->memlen) - (qposi - hi.memInfo->memlen);
                    auto rdiff = rposi - rposj;

                    auto extensionScore = f[j] + alpha(qdiff, rdiff, hi.memInfo->memlen,
                                                       hi.memInfo->readEnd != hj.memInfo->readEnd) -
                                          beta(qdiff, rdiff, avgseed,
                                               hi.memInfo->readEnd !=
                                               hj.memInfo->readEnd,
                                               hi.isFw == hj.isFw,
                                               hi.memInfo->memlen);
                    if (verbose) {
                        std::cerr << i << " " << j <<
                                  " f[i]:" << f[i] << " f[j]:" << f[j] <<
                                  " sameRead:" << (uint32_t) (hi.memInfo->readEnd == hj.memInfo->readEnd) <<
                                  " ori:" << (uint32_t) hi.isFw << " sameOri:" << (uint32_t) (hi.isFw == hj.isFw) <<
                                  " ri, j:" << rposi << " " << rposj <<
                                  " qi,j:" << qposi << " " << qposj <<
                                  " readDiff:" << qdiff << " refDiff:" << rdiff <<
                                  " len:" << hi.memInfo->memlen <<
                                  " alpha:"
                                  << alpha(qdiff, rdiff, hi.memInfo->memlen, hi.memInfo->readEnd != hj.memInfo->readEnd)
                                  <<
                                  " beta:" << beta(qdiff, rdiff, avgseed,
                                                   hi.memInfo->readEnd !=
                                                   hj.memInfo->readEnd,
                                                   hi.isFw == hj.isFw,
                                                   hi.memInfo->memlen) <<
                                  " extensionScore: " << extensionScore << "\n";
                    }
                    bool extendWithJ = (extensionScore > f[i]);
                    p[i] = extendWithJ ? j : p[i];
                    f[i] = extendWithJ ? extensionScore : f[i];
                    // HEURISTIC : if we connected this match to an earlier one
                    // i.e. if we extended the chain.
                    // This implements Heng Li's heuristic ---
                    // "
                    // We note that if anchor i is chained to j, chaining i to a predecessor of j
                    // is likely to yield a lower score.
                    // "
                    // here we take this to the extreme, and stop at the first j to which we chain.
                    // we can add a parameter "h" as in the minimap paper.  But here we expect the
                    // chains of matches in short reads to be short enough that this may not be worth it.
                    if (p[i] < i) {
                        numRounds--;
                        if (numRounds <= 0) { break; }
                    }
                }
                if (f[i] > bestScore) {
                    bestScore = f[i];
                    //bestChainEnd = i;
                    bestChainEndList.clear();
                    bestChainEndList.push_back(i);
                } else if (f[i] == bestScore) {
                    bestChainEndList.push_back(i);
                }
            }

            // Do backtracking
            std::vector<bool> seen(f.size());
            for (uint64_t i = 0; i < seen.size(); i++) seen[i] = false;
            for (auto bc : bestChainEndList) {
                int32_t bestChainEnd = bc;
                bool shouldBeAdded = true;
                if (bestChainEnd >= 0) {
                    std::vector<uint64_t> memIndicesInReverse;
                    auto lastPtr = p[bestChainEnd];
                    while (lastPtr < bestChainEnd) {
                        if (seen[bestChainEnd]) {
                            shouldBeAdded = false;
                            break;
                        }
                        memIndicesInReverse.push_back(bestChainEnd);
                        seen[bestChainEnd] = true;
                        bestChainEnd = lastPtr;
                        lastPtr = p[bestChainEnd];
//                    lastPtr = bestChainEnd;
//                    bestChainEnd = p[bestChainEnd];
                    }
                    if (seen[bestChainEnd]) {
                        shouldBeAdded = false;
                    }
                    memIndicesInReverse.push_back(bestChainEnd);
                    if (shouldBeAdded) {
                        all.push_back(util::MemCluster(isFw));
                        all.push_back(util::MemCluster(isFw));
                        std::vector<util::MemCluster>::iterator lclust = all.end() - 2;
                        std::vector<util::MemCluster>::iterator rclust = all.end() - 1;
                        for (auto it = memIndicesInReverse.rbegin(); it != memIndicesInReverse.rend(); it++) {
                            if (memList[*it].memInfo->readEnd == util::ReadEnd::LEFT) {
                                if (verbose) {
                                    std::cerr << "wl-" << *it << ":" << memList[*it].tpos << " " << memList[*it].isFw
                                              << "\n";
                                }
                                lclust->addMem(memList[*it].memInfo, memList[*it].tpos, memList[*it].isFw);
                                lclust->isFw = memList[*it].isFw;
                            } else {
                                if (verbose) {
                                    std::cerr << "wr-" << *it << ":" << memList[*it].tpos << " " << memList[*it].isFw
                                              << "\n";
                                }
                                rclust->addMem(memList[*it].memInfo, memList[*it].tpos, memList[*it].isFw);
                                rclust->isFw = memList[*it].isFw;
                            }
                        }

                        //FIXME take care of the left and right coverage/score separately!!
                        lclust->coverage = bestScore / 2.0;
                        rclust->coverage = bestScore / 2.0;

                        if (bestScore > globalScore) {
                            globalScore = bestScore;
                        }
                        uint32_t fragmentLen, fragStart, fragEnd;
                        fragStart = lclust->firstRefPos();
                        if (rclust->firstRefPos() < fragStart) {
                            fragStart = rclust->firstRefPos();
                        }
                        fragEnd = rclust->lastRefPos() + rclust->lastMemLen();
                        if (fragEnd < lclust->lastRefPos() + lclust->lastMemLen()) {
                            fragEnd = lclust->lastRefPos() + lclust->lastMemLen();
                        }
                        fragmentLen = fragEnd - fragStart;
                        if (verbose) {
                            std::cerr << "fragmentlen: " << fragmentLen << "\n";
                        }

                        if (all.size() >= 200 * 2 * trMemMap.size()) {
                            std::cerr << "BAD; SHOULD NOT HAPPEN! " << all.size() << "\n";
                            std::exit(1);
                        }
                        if (!rclust->mems.empty() and !lclust->mems.empty() and
                            fragmentLen < maxFragmentLength) {
                            jointMemsList.emplace_back(tid, lclust, rclust, fragmentLen);
                        } else {
//                            std::cerr << "in else\n";
                            if (!lclust->mems.empty())
                                jointMemsList.emplace_back(tid, lclust, all.begin(), fragmentLen);
                            if (!rclust->mems.empty())
                                jointMemsList.emplace_back(tid, all.begin(), rclust, fragmentLen);
                            if (bestScore == globalScore) {
                                globalScore = bestScore / 2.0;
                            }

                        } /*else {
                            std::cerr << "fragmentlen issue: " << lclust->lastRefPos()
                                      << " " << lclust->lastMemLen() << " "
                                                                        << rclust->firstRefPos() << " " << maxFragmentLength << "\n";
                        }*/
                    }

//                minPosIt += lastPtr;
                } else {
                    // should not happen
                    std::cerr << "[FATAL] : Cannot find any valid chain for quasi-mapping\n";
                    std::cerr << "num hits = " << memList.size() << "\n";
                    std::cerr << "bestChainEnd = " << bestChainEnd << "\n";
                    std::cerr << "bestChainScore = " << bestScore << "\n";
                    std::exit(1);
                }
            }

        }
        bool yes = false;
        double tmpScore;
        if (jointMemsList.size() > 0) {
            yes = true;
            tmpScore = jointMemsList.front().coverage();
        } else if (!no) {
            std::cerr << "This is even worse than F*\n";
            std::exit(1);
        }
        jointMemsList.erase(std::remove_if(jointMemsList.begin(), jointMemsList.end(),
                                           [&globalScore](util::JointMems &pairedReadMems) -> bool {
                                               return pairedReadMems.coverage() < (globalScore - 0.01);
                                           }),
                            jointMemsList.end());
        if (jointMemsList.size() == 0 and yes) {
            std::cerr << "WTF IS GOING ON\n" << tmpScore << " " << globalScore << "\n";
            std::exit(1);
        }
        /*if (jointMemsList.size() >= 7) {
            uint32_t cntr=0;
            for (auto &j : jointMemsList) {
                std::cerr << "\n" << cntr++ << " --> ";
                std::cerr << "tid:" << j.tid << "\n" ;
                std::cerr << "left:" << j.leftClust->isFw << " size:" << j.leftClust->mems.size() << " score:" << j.leftClust->coverage << " --> ";
                for (size_t i = 0; i < j.leftClust->mems.size(); i++) {
                    std::cerr << "t" << j.leftClust->mems[i].tpos <<
                              " r" << j.leftClust->mems[i].memInfo->rpos <<
                              " l" << j.leftClust->mems[i].memInfo->memlen << "\t";
                }
                std::cerr << "\nright:" << j.rightClust->isFw << " size:" << j.rightClust->mems.size() << " score:" << j.rightClust->coverage << " --> ";
                for (size_t i = 0; i < j.rightClust->mems.size(); i++) {
                    std::cerr << "t" << j.rightClust->mems[i].tpos <<
                              " r" << j.rightClust->mems[i].memInfo->rpos <<
                              " l" << j.rightClust->mems[i].memInfo->memlen << "\t";
                }
            }
        }*/
        return true;
    }


    bool findOptChain(std::vector<std::pair<int, util::ProjectedHits>> &hits,
                      spp::sparse_hash_map<pufferfish::common_types::ReferenceID, std::vector<util::MemCluster>> &memClusters,
                      uint32_t maxSpliceGap, std::vector<util::UniMemInfo> &memCollection, bool verbose = false) {


        using namespace pufferfish::common_types;
        //(void)verbose;

        // Map from (reference id, orientation) pair to a cluster of MEMs.
        std::map<std::pair<ReferenceID, bool>, std::vector<util::MemInfo>>
                trMemMap;
        if (!fillMemCollection(hits, trMemMap, memCollection, util::ReadEnd::LEFT, verbose))
            return false;

        std::vector<double> f;
        std::vector<int32_t> p;

        for (auto &trMem : core::range<decltype(trMemMap.begin())>(trMemMap.begin(), trMemMap.end())) {
            auto &trOri = trMem.first;
            auto &tid = trOri.first;
            auto &isFw = trOri.second;
            auto &memList = trMem.second;
            // sort memList according to mem reference positions
            std::sort(memList.begin(), memList.end(),
                      [isFw](util::MemInfo &q1, util::MemInfo &q2) -> bool {
                          auto q1ref = q1.tpos + q1.memInfo->memlen;
                          auto q2ref = q2.tpos + q2.memInfo->memlen;
                          auto q1read = q1.memInfo->rpos + q1.memInfo->memlen;
                          auto q2read = q2.memInfo->rpos + q2.memInfo->memlen;
                          return q1ref != q2ref ? q1ref < q2ref :
                                 (isFw ? q1read < q2read : q1read > q2read);// sort based on tpos
                      });
            if (verbose) {
                std::cerr << "\ntid" << tid << /*pfi_->refName(tid) << */" , isFw:" << isFw << "\n";
                for (auto &m : memList) {
                    std::cerr << "\ttpos:" << m.tpos << " rpos:" << m.memInfo->rpos << " len:" << m.memInfo->memlen
                              << "\n";
                }
            }

            //auto minPosIt = memList.begin();
            // find the valid chains
            // Use variant of minimap2 scoring (Li 2018)
            // https://academic.oup.com/bioinformatics/advance-article/doi/10.1093/bioinformatics/bty191/4994778
            auto alpha = [](int32_t qdiff, int32_t rdiff, int32_t ilen) -> double {
                double score = ilen;
                double mindiff = (qdiff < rdiff) ? qdiff : rdiff;
                return (score < mindiff) ? score : mindiff;
            };

            auto beta = [maxSpliceGap](int32_t qdiff, int32_t rdiff, double avgseed) -> double {
                if (qdiff < 0 or ((uint32_t) std::max(qdiff, rdiff) > maxSpliceGap)) {
                    return std::numeric_limits<double>::infinity();
                }
                double l = qdiff - rdiff;
                int32_t al = std::abs(l);
                return (l == 0) ? 0.0 : (0.01 * avgseed * al + 0.5 * fastlog2(static_cast<float>(al)));
            };
            double bottomScore = std::numeric_limits<double>::lowest();
            double bestScore = bottomScore;
            int32_t bestChainEnd = -1;
            std::vector<int32_t> bestChainEndList;
            double avgseed = 31.0;
            f.clear();
            p.clear();
            //auto lastHitId = static_cast<int32_t>(memList.size() - 1);
            for (int32_t i = 0; i < static_cast<int32_t>(memList.size()); ++i) {
                auto &hi = memList[i];

                auto qposi = hi.memInfo->rpos + hi.memInfo->memlen;
                auto rposi = hi.tpos + hi.memInfo->memlen;

                double baseScore = static_cast<double>(hi.memInfo->memlen);
                p.push_back(i);
                f.push_back(baseScore);

                // possible predecessors in the chain
                int32_t numRounds{2};
                (void) numRounds;
                for (int32_t j = i - 1; j >= 0; --j) {
                    auto &hj = memList[j];

                    auto qposj = hj.memInfo->rpos + hj.memInfo->memlen;
                    auto rposj = hj.tpos + hj.memInfo->memlen;

                    auto qdiff = isFw ? qposi - qposj :
                                 (qposj - hj.memInfo->memlen) - (qposi - hi.memInfo->memlen);
                    auto rdiff = rposi - rposj;

                    auto extensionScore = f[j] + alpha(qdiff, rdiff, hi.memInfo->memlen) - beta(qdiff, rdiff, avgseed);
                    if (verbose) {
                        std::cerr << i << " " << j <<
                                  " f[i]:" << f[i] << " f[j]:" << f[j] <<
                                  " readDiff:" << qdiff << " refDiff:" << rdiff <<
                                  " alpha:" << alpha(qdiff, rdiff, hi.memInfo->memlen) <<
                                  " beta:" << beta(qdiff, rdiff, avgseed) <<
                                  " extensionScore: " << extensionScore << "\n";
                    }
                    bool extendWithJ = (extensionScore > f[i]);
                    p[i] = extendWithJ ? j : p[i];
                    f[i] = extendWithJ ? extensionScore : f[i];
                    // HEURISTIC : if we connected this match to an earlier one
                    // i.e. if we extended the chain.
                    // This implements Heng Li's heuristic ---
                    // "
                    // We note that if anchor i is chained to j, chaining i to a predecessor of j
                    // is likely to yield a lower score.
                    // "
                    // here we take this to the extreme, and stop at the first j to which we chain.
                    // we can add a parameter "h" as in the minimap paper.  But here we expect the
                    // chains of matches in short reads to be short enough that this may not be worth it.
                    if (p[i] < i) {
                        numRounds--;
                        if (numRounds <= 0) { break; }
                    }
                }
                if (f[i] > bestScore) {
                    bestScore = f[i];
                    bestChainEnd = i;
                    bestChainEndList.clear();
                    bestChainEndList.push_back(bestChainEnd);
                } else if (f[i] == bestScore) {
                    bestChainEndList.push_back(i);
                }
            }



            // Do backtracking
            std::vector<bool> seen(f.size());
            for (uint64_t i = 0; i < seen.size(); i++) seen[i] = false;
            for (auto bestChainEnd : bestChainEndList) {
                bool shouldBeAdded = true;
                if (bestChainEnd >= 0) {
                    std::vector<uint64_t> memIndicesInReverse;
                    auto lastPtr = p[bestChainEnd];
                    while (lastPtr < bestChainEnd) {
                        if (seen[bestChainEnd]) {
                            shouldBeAdded = false;
                            break;
                        }
                        memIndicesInReverse.push_back(bestChainEnd);
                        seen[bestChainEnd] = true;
                        bestChainEnd = lastPtr;
                        lastPtr = p[bestChainEnd];
//                    lastPtr = bestChainEnd;
//                    bestChainEnd = p[bestChainEnd];
                    }
                    if (seen[bestChainEnd]) {
                        shouldBeAdded = false;
                    }
                    memIndicesInReverse.push_back(bestChainEnd);
                    if (shouldBeAdded) {
                        memClusters[tid].insert(memClusters[tid].begin(), util::MemCluster(isFw));
                        for (auto it = memIndicesInReverse.rbegin(); it != memIndicesInReverse.rend(); it++) {
                            memClusters[tid][0].addMem(memList[*it].memInfo,
                                                       memList[*it].tpos);
                        }
                        memClusters[tid][0].coverage = bestScore;
                    }
//                minPosIt += lastPtr;
                } else {
                    // should not happen
                    std::cerr << "[FATAL] : Cannot find any valid chain for quasi-mapping\n";
                    std::cerr << "num hits = " << memList.size() << "\n";
                    std::cerr << "bestChainEnd = " << bestChainEnd << "\n";
                    std::cerr << "bestChainScore = " << bestScore << "\n";
                    std::exit(1);
                }
            }

        }
        if (verbose)
            std::cerr << "\n[END OF FIND_OPT_CHAIN]\n";

        return true;
    }

};

#endif //PUFFERFISH_CHAINFINDER_H
