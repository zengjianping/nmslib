/**
 * Non-metric Space Library
 *
 * Authors: Bilegsaikhan Naidan (https://github.com/bileg), Leonid Boytsov (http://boytsov.info).
 * With contributions from Lawrence Cayton (http://lcayton.com/) and others.
 *
 * For the complete list of contributors and further details see:
 * https://github.com/searchivarius/NonMetricSpaceLib
 *
 * Copyright (c) 2014
 *
 * This code is released under the
 * Apache License Version 2.0 http://www.apache.org/licenses/.
 *
 */
#include <cmath>
#include <memory>
#include <iostream>

#include "space.h"
#include "knnquery.h"
#include "rangequery.h"
#include "ported_boost_progress.h"
#include "method/small_world_rand.h"

#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <typeinfo>

#ifdef _OPENMP
#include <omp.h>
#endif

#define USE_BITSET_FOR_INDEXING 1

namespace similarity {

template <typename dist_t>
struct IndexThreadParamsSW {
  const Space<dist_t>*                        space_;
  SmallWorldRand<dist_t>&                     index_;
  const ObjectVector&                         data_;
  size_t                                      index_every_;
  size_t                                      out_of_;
  ProgressDisplay*                            progress_bar_;
  mutex&                                      display_mutex_;
  size_t                                      progress_update_qty_;
  
  IndexThreadParamsSW(
                     const Space<dist_t>*             space,
                     SmallWorldRand<dist_t>&          index, 
                     const ObjectVector&              data,
                     size_t                           index_every,
                     size_t                           out_of,
                     ProgressDisplay*                 progress_bar,
                     mutex&                           display_mutex,
                     size_t                           progress_update_qty
                      ) : 
                     space_(space),
                     index_(index), 
                     data_(data),
                     index_every_(index_every),
                     out_of_(out_of),
                     progress_bar_(progress_bar),
                     display_mutex_(display_mutex),
                     progress_update_qty_(progress_update_qty)
                     { }
};

template <typename dist_t>
struct IndexThreadSW {
  void operator()(IndexThreadParamsSW<dist_t>& prm) {
    ProgressDisplay*  progress_bar = prm.progress_bar_;
    mutex&            display_mutex(prm.display_mutex_); 
    /* 
     * Skip the first element, it was added already
     */
    size_t nextQty = prm.progress_update_qty_;
    for (size_t i = 1; i < prm.data_.size(); ++i) {
      if (prm.index_every_ == i % prm.out_of_) {
        MSWNode* node = new MSWNode(prm.data_[i]);
        prm.index_.add(prm.space_, node);
      
        if ((i + 1 >= min(prm.data_.size(), nextQty)) && progress_bar) {
          unique_lock<mutex> lock(display_mutex);
          (*progress_bar) += (nextQty - progress_bar->count());
          nextQty += prm.progress_update_qty_;
        }
      }
    }
    if (progress_bar) {
      unique_lock<mutex> lock(display_mutex);
      (*progress_bar) += (progress_bar->expected_count() - progress_bar->count());
    }
  }
};

template <typename dist_t>
SmallWorldRand<dist_t>::SmallWorldRand(bool PrintProgress,
                                       const Space<dist_t>* space,
                                       const ObjectVector& data,
                                       const AnyParams& MethParams) :
                                                   NN_(5),
                                                   initIndexAttempts_(2),
                                                   initSearchAttempts_(10),
                                                   indexThreadQty_(0)
{
  AnyParamManager pmgr(MethParams);

#ifdef _OPENMP
  indexThreadQty_ = omp_get_max_threads();
#endif

  pmgr.GetParamOptional("NN", NN_);
  pmgr.GetParamOptional("initIndexAttempts",  initIndexAttempts_);
  pmgr.GetParamOptional("initSearchAttempts", initSearchAttempts_);
  pmgr.GetParamOptional("indexThreadQty",     indexThreadQty_);

  LOG(LIB_INFO) << "NN                  = " << NN_;
  LOG(LIB_INFO) << "initIndexAttempts   = " << initIndexAttempts_;
  LOG(LIB_INFO) << "initSearchAttempts  = " << initSearchAttempts_;
  LOG(LIB_INFO) << "indexThreadQty      = " << indexThreadQty_;

  if (data.empty()) return;

  // 1) During indexing: Don't create an MSWNode without adding it
  // The field addIndex_ will not be init properly!
  // 2) One entry should be added before all the threads are started, or else add() might not work properly
  addCriticalSection(new MSWNode(data[0]));

  unique_ptr<ProgressDisplay> progress_bar(PrintProgress ?
                                new ProgressDisplay(data.size(), cerr)
                                :NULL);

  if (indexThreadQty_ <= 1) {
    // Skip the first element, one element is already added
    if (progress_bar) ++(*progress_bar);
    for (size_t i = 1; i != data.size(); ++i) {
      MSWNode* node = new MSWNode(data[i]);
      add(space, node);
      if (progress_bar) ++(*progress_bar);
    }
  } else {
    vector<thread>                                    threads(indexThreadQty_);
    vector<shared_ptr<IndexThreadParamsSW<dist_t>>>   threadParams; 
    mutex                                             progressBarMutex;

    for (size_t i = 0; i < indexThreadQty_; ++i) {
      threadParams.push_back(shared_ptr<IndexThreadParamsSW<dist_t>>(
                              new IndexThreadParamsSW<dist_t>(space, *this, data, i, indexThreadQty_,
                                                              progress_bar.get(), progressBarMutex, 200)));
    }
    for (size_t i = 0; i < indexThreadQty_; ++i) {
      threads[i] = thread(IndexThreadSW<dist_t>(), ref(*threadParams[i]));
    }
    for (size_t i = 0; i < indexThreadQty_; ++i) {
      threads[i].join();
    }
    LOG(LIB_INFO) << indexThreadQty_ << " indexing threads have finished";
  }
}

template <typename dist_t>
void 
SmallWorldRand<dist_t>::SetQueryTimeParamsInternal(AnyParamManager& pmgr) {
  pmgr.GetParamOptional("initSearchAttempts", initSearchAttempts_);
}

template <typename dist_t>
vector<string>
SmallWorldRand<dist_t>::GetQueryTimeParamNames() const {
  vector<string> names;
  names.push_back("initSearchAttempts");
  return names;
}

template <typename dist_t>
const std::string SmallWorldRand<dist_t>::ToString() const {
  return "small_world_rand";
}

template <typename dist_t>
SmallWorldRand<dist_t>::~SmallWorldRand() {
}

template <typename dist_t>
MSWNode* SmallWorldRand<dist_t>::getRandomEntryPointLocked() const
{
  unique_lock<mutex> lock(ElListGuard_);
  MSWNode* res = getRandomEntryPoint();
  return res;
}

template <typename dist_t>
size_t SmallWorldRand<dist_t>::getEntryQtyLocked() const
{
  unique_lock<mutex> lock(ElListGuard_);
  size_t res = ElList_.size();
  return res;
}

template <typename dist_t>
MSWNode* SmallWorldRand<dist_t>::getRandomEntryPoint() const {
  size_t size = ElList_.size();

  if(!ElList_.size()) {
    return NULL;
  } else {
    size_t num = rand()%size;
    return ElList_[num];
  }
}

template <typename dist_t>
void 
SmallWorldRand<dist_t>::kSearchElementsWithAttempts(const Space<dist_t>* space, 
                                                    const Object* queryObj, 
                                                    size_t NN, 
                                                    size_t initIndexAttempts,
                                                    priority_queue<EvaluatedMSWNodeDirect<dist_t>>& resultSet) const
{
  size_t                              entryQty = getEntryQtyLocked();
#if USE_BITSET_FOR_INDEXING
  vector<bool>                        visitedBitset(entryQty); // seems to be working fine even in a multi-threaded mode.
#else
  unordered_set<MSWNode*>             visited;
#endif

  for (size_t i=0; i < initIndexAttempts; i++){

    /**
     * Search for the k most closest elements to the query.
     */
    MSWNode* provider = getRandomEntryPointLocked();

    priority_queue <dist_t>                     closestDistQueue;                      
    priority_queue <EvaluatedMSWNodeReverse<dist_t>>   candidateSet; 

    dist_t d = space->IndexTimeDistance(queryObj, provider->getData());
    EvaluatedMSWNodeReverse<dist_t> ev(d, provider);

    candidateSet.push(ev);
    closestDistQueue.push(d);
 
#if USE_BITSET_FOR_INDEXING
    /* 
     * Recall that
     * 1) Some entries might not have their addIndex_ initialized (in this case addIndex_ will be a very large value)
     * 2) Some entries might have been added after the call to getEntryQtyLocked();
     */
    if (provider->addIndex_ < entryQty) visitedBitset[provider->addIndex_] = true;
#else
    visited.insert(provider);
#endif
    resultSet.emplace(d, provider);

    while (!candidateSet.empty()) {
      const EvaluatedMSWNodeReverse<dist_t>& currEv = candidateSet.top();
      dist_t lowerBound = closestDistQueue.top();

      /*
       * Check if we reached a local minimum.
       */
      if (currEv.getDistance() > lowerBound) {
        break;
      }
      MSWNode* currNode = currEv.getMSWNode();

      /*
       * This lock protects currNode from being modified
       * while we are accessing elements of currNode.
       */
      unique_lock<mutex>  lock(currNode->accessGuard_);

      const vector<MSWNode*>& neighbor = currNode->getAllFriends();

      // Can't access curEv anymore! The reference would become invalid
      candidateSet.pop();

      // calculate distance to each neighbor
      for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter){
        size_t nodeAddIndex = (*iter)->addIndex_;
#if USE_BITSET_FOR_INDEXING
    /*
     *  Actually such a situation is quite normal, because:
     * 1) Some entries might not have their addIndex_ initialized (in this case addIndex_ will be a very large value)
     * 2) Some entries might have been added after the call to getEntryQtyLocked();
        However, in such a case the condition nodeAddIndex >= entryQty will hold true.

        if (nodeAddIndex == numeric_limits<size_t>::max()) {
          LOG(LIB_INFO) << "Bug: uninitialized addIndex_";
          throw runtime_error("Bug: uninitialized addIndex_");
        }
     */
        if (nodeAddIndex >= entryQty || !visitedBitset[nodeAddIndex]) {
          if (nodeAddIndex < entryQty) visitedBitset[nodeAddIndex] = true;
#else
        if (visited.find((*iter)) == visited.end()) {
          visited.insert(*iter);
#endif
          d = space->IndexTimeDistance(queryObj, (*iter)->getData());


          closestDistQueue.push(d);
          if (closestDistQueue.size() > NN) {
            closestDistQueue.pop();
          }
          EvaluatedMSWNodeReverse<dist_t> evE1(d, *iter);
          candidateSet.push(evE1);
          if (resultSet.size() < NN || resultSet.top().getDistance() > d) {
            resultSet.emplace(d, *iter);
            if (resultSet.size() > NN) { // TODO check somewhere that NN > 0
              resultSet.pop();
            }
          }
        }
      }
    }
  }
}


template <typename dist_t>
void SmallWorldRand<dist_t>::add(const Space<dist_t>* space, MSWNode *newElement){
  newElement->removeAllFriends(); 

  bool isEmpty = false;

  {
    unique_lock<mutex> lock(ElListGuard_);
    isEmpty = ElList_.empty();
  }

  if(isEmpty){
  // Before add() is called, the first node should be created!
    LOG(LIB_INFO) << "Bug: the list of nodes shouldn't be empty!";
    throw runtime_error("Bug: the list of nodes shouldn't be empty!");
  }

  {
    priority_queue<EvaluatedMSWNodeDirect<dist_t>> resultSet;

    kSearchElementsWithAttempts(space, newElement->getData(), NN_, initIndexAttempts_, resultSet);

    // TODO actually we might need to add elements in the reverse order in the future.
    // For the current implementation, however, the order doesn't seem to matter
    while (!resultSet.empty()) {
      link(resultSet.top().getMSWNode(), newElement);
      resultSet.pop();
    }
  }

  addCriticalSection(newElement);
}

template <typename dist_t>
void SmallWorldRand<dist_t>::addCriticalSection(MSWNode *newElement){
  unique_lock<mutex> lock(ElListGuard_);

  newElement->addIndex_ = ElList_.size(); // Need to do this under the mutex protection
  ElList_.push_back(newElement);
}

template <typename dist_t>
void SmallWorldRand<dist_t>::Search(RangeQuery<dist_t>* query) {
  throw runtime_error("Range search is not supported!");
}

template <typename dist_t>
void SmallWorldRand<dist_t>::Search(KNNQuery<dist_t>* query) {
  vector<bool>                        visitedBitset(ElList_.size());

  for (size_t i=0; i < initSearchAttempts_; i++) {
  /**
   * Search of most k-closest elements to the query.
   */
    MSWNode* provider = getRandomEntryPoint();

    priority_queue <dist_t>                   closestDistQueue; //The set of all elements which distance was calculated
    priority_queue <EvaluatedMSWNodeReverse<dist_t>> candidateQueue; //the set of elements which we can use to evaluate

    dist_t d = query->DistanceObjLeft(provider->getData());

    EvaluatedMSWNodeReverse<dist_t> ev(d, provider);
    candidateQueue.push(ev);
    closestDistQueue.emplace(d);

    if (provider->addIndex_ == numeric_limits<size_t>::max()) {
      LOG(LIB_INFO) << "Bug: uninitialized addIndex_";
      throw runtime_error("Bug: uninitialized addIndex_");
    }

    visitedBitset[provider->addIndex_] = true; 

    while(!candidateQueue.empty()){

      auto iter = candidateQueue.top();
      const EvaluatedMSWNodeReverse<dist_t>& currEv = iter;
 
      dist_t lowerBound = closestDistQueue.top();

      // Did we reach a local minimum?
      if (currEv.getDistance() > lowerBound) {
        break;
      }

      const vector<MSWNode*>& neighbor = (currEv.getMSWNode())->getAllFriends();

      // Can't access curEv anymore! The reference would become invalid
      candidateQueue.pop();

      //calculate distance to each neighbor
      for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter){
        size_t nodeAddIndex = (*iter)->addIndex_;
        if (nodeAddIndex == numeric_limits<size_t>::max()) {
          LOG(LIB_INFO) << "Bug: uninitialized addIndex_";
          throw runtime_error("Bug: uninitialized addIndex_");
        }
        if (!visitedBitset[nodeAddIndex]) {
          const Object* currObj = (*iter)->getData();
          d = query->DistanceObjLeft(currObj);
            
          visitedBitset[nodeAddIndex] = true;
          closestDistQueue.emplace(d);
          if (closestDistQueue.size() > NN_) { 
            closestDistQueue.pop(); 
          }
          candidateQueue.emplace(d, *iter);
          query->CheckAndAddToResult(d, currObj);
        }
      }
    }
  }
}



template class SmallWorldRand<float>;
template class SmallWorldRand<double>;
template class SmallWorldRand<int>;

}
