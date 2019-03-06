// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file PublisherHistory.cpp
 *
 */

#include <mutex>

#include <fastrtps/publisher/PublisherHistory.h>

#include "PublisherImpl.h"

#include <fastrtps/rtps/writer/RTPSWriter.h>

#include <fastrtps/log/Log.h>

#include <mutex>

extern eprosima::fastrtps::rtps::WriteParams WRITE_PARAM_DEFAULT;

using namespace eprosima::fastrtps;
using namespace eprosima::fastrtps::rtps;

PublisherHistory::PublisherHistory(
        PublisherImpl* pimpl,
        uint32_t payloadMaxSize,
        const HistoryQosPolicy& history,
        const ResourceLimitsQosPolicy& resource,
        MemoryManagementPolicy_t mempolicy)
    : WriterHistory(HistoryAttributes(mempolicy, payloadMaxSize,
                history.kind == KEEP_ALL_HISTORY_QOS ?
                        resource.allocated_samples :
                        pimpl->getAttributes().topic.getTopicKind() == NO_KEY ?
                            std::min(resource.allocated_samples, history.depth) :
                            std::min(resource.allocated_samples, history.depth * resource.max_instances),
                history.kind == KEEP_ALL_HISTORY_QOS ?
                        resource.max_samples :
                        pimpl->getAttributes().topic.getTopicKind() == NO_KEY ?
                            history.depth :
                            history.depth * resource.max_instances))
    , m_historyQos(history)
    , m_resourceLimitsQos(resource)
    , mp_pubImpl(pimpl)
    , mp_latestCacheChange(new CacheChange_t)
{
    // TODO Auto-generated constructor stub

}

PublisherHistory::~PublisherHistory()
{
    delete mp_latestCacheChange;
}


bool PublisherHistory::add_pub_change(
        CacheChange_t* change,
        WriteParams &wparams,
        std::unique_lock<std::recursive_mutex>& lock)
{
    if(m_isHistoryFull)
    {
        bool ret = false;

        if(m_historyQos.kind == KEEP_ALL_HISTORY_QOS)
        {
            ret = this->mp_pubImpl->try_remove_change(lock);
        }
        else if(m_historyQos.kind == KEEP_LAST_HISTORY_QOS)
        {
            ret = this->remove_min_change();
        }

        if(!ret)
        {
            logWarning(RTPS_HISTORY,"Attempting to add Data to Full WriterCache: "<<this->mp_pubImpl->getGuid().entityId);
            return false;
        }
    }

    assert(!m_isHistoryFull);

    bool returnedValue = false;

    //NO KEY HISTORY
    if(mp_pubImpl->getAttributes().topic.getTopicKind() == NO_KEY)
    {
        if(this->add_change(change, wparams))
        {
            mp_latestCacheChange->copy(change);
            returnedValue = true;
        }
    }
    //HISTORY WITH KEY
    else if(mp_pubImpl->getAttributes().topic.getTopicKind() == WITH_KEY)
    {
        t_m_Inst_Caches::iterator vit;
        if(find_key(change,&vit))
        {
            logInfo(RTPS_HISTORY,"Found key: "<< vit->first);
            bool add = false;
            if(m_historyQos.kind == KEEP_ALL_HISTORY_QOS)
            {
                if((int32_t)vit->second.size() < m_resourceLimitsQos.max_samples_per_instance)
                {
                    add = true;
                }
                else
                {
                    logWarning(RTPS_HISTORY,"Change not added due to maximum number of samples per instance"<<endl;);
                }
            }
            else if (m_historyQos.kind == KEEP_LAST_HISTORY_QOS)
            {
                if(vit->second.size() < (size_t)m_historyQos.depth)
                {
                    add = true;
                }
                else
                {
                    if(remove_change_pub(vit->second.front()))
                    {
                        add = true;
                    }
                }
            }

            if(add)
            {
                if(this->add_change(change, wparams))
                {
                    logInfo(RTPS_HISTORY,this->mp_pubImpl->getGuid().entityId <<" Change "
                            << change->sequenceNumber << " added with key: "<<change->instanceHandle
                            << " and "<<change->serializedPayload.length<< " bytes");
                    vit->second.push_back(change);
                    returnedValue =  true;
                }
            }
        }
    }


    return returnedValue;
}

bool PublisherHistory::find_key(
        CacheChange_t* a_change,
        t_m_Inst_Caches::iterator* vit_out)
{
    t_m_Inst_Caches::iterator vit;
    vit = m_keyedChanges.find(a_change->instanceHandle);
    if (vit != m_keyedChanges.end())
    {
        *vit_out = vit;
        return true;
    }

    if ((int)m_keyedChanges.size() < m_resourceLimitsQos.max_instances)
    {
        *vit_out = m_keyedChanges.insert(std::make_pair(a_change->instanceHandle, std::vector<CacheChange_t*>())).first;
        return true;
    }
    else
    {
        for (vit = m_keyedChanges.begin(); vit != m_keyedChanges.end(); ++vit)
        {
            if (vit->second.size() == 0)
            {
                m_keyedChanges.erase(vit);
                *vit_out = m_keyedChanges.insert(std::make_pair(a_change->instanceHandle, std::vector<CacheChange_t*>())).first;
                return true;
            }
        }
        logWarning(PUBLISHER, "History has reached the maximum number of instances" << endl;)
    }
    return false;
}


bool PublisherHistory::removeAllChange(size_t* removed)
{

    size_t rem = 0;
    std::lock_guard<std::recursive_mutex> guard(*this->mp_mutex);

    while(m_changes.size()>0)
    {
        if(remove_change_pub(m_changes.front()))
            ++rem;
        else
            break;
    }
    if(removed!=nullptr)
        *removed = rem;
    if (rem>0)
        return true;
    return false;
}


bool PublisherHistory::removeMinChange()
{
    if(mp_writer == nullptr || mp_mutex == nullptr)
    {
        logError(RTPS_HISTORY,"You need to create a Writer with this History before using it");
        return false;
    }

    std::lock_guard<std::recursive_mutex> guard(*this->mp_mutex);
    if(m_changes.size()>0)
        return remove_change_pub(m_changes.front());
    return false;
}

bool PublisherHistory::remove_change_pub(CacheChange_t* change)
{

    if(mp_writer == nullptr || mp_mutex == nullptr)
    {
        logError(RTPS_HISTORY,"You need to create a Writer with this History before using it");
        return false;
    }

    std::lock_guard<std::recursive_mutex> guard(*this->mp_mutex);
    if(mp_pubImpl->getAttributes().topic.getTopicKind() == NO_KEY)
    {
        if(this->remove_change(change))
        {
            m_isHistoryFull = false;
            return true;
        }

        return false;
    }
    else
    {
        t_m_Inst_Caches::iterator vit;
        if(!this->find_key(change,&vit))
        {
            return false;
        }

        for(auto chit = vit->second.begin(); chit!= vit->second.end(); ++chit)
        {
            if( ((*chit)->sequenceNumber == change->sequenceNumber)
                    && ((*chit)->writerGUID == change->writerGUID) )
            {
                if(remove_change(change))
                {
                    vit->second.erase(chit);
                    m_isHistoryFull = false;
                    return true;
                }
            }
        }
        logError(PUBLISHER,"Change not found, something is wrong");
    }
    return false;
}

bool PublisherHistory::remove_change_g(CacheChange_t* a_change)
{
    return remove_change_pub(a_change);
}

void PublisherHistory::get_latest_samples(std::vector<CacheChange_t *> &samples, int& num_samples)
{
    num_samples = 0;

    if (mp_pubImpl->getAttributes().topic.getTopicKind() == NO_KEY)
    {
        if (samples.size() < 1)
        {
            logError(PUBLISHER, "Cannot return latest sample, output vector is not long enough");
            return;
        }

        if (mp_latestCacheChange != mp_invalidCache)
        {
            samples[num_samples++] = mp_latestCacheChange;
        }
    }
    else if (mp_pubImpl->getAttributes().topic.getTopicKind() == WITH_KEY)
    {
        if (samples.size() < m_keyedChanges.size())
        {
            logError(PUBLISHER, "Cannot return latest samples, output vector is not long enough");
            return;
        }

        if (m_keyedChanges.empty())
        {
            return;
        }

        for (auto it = m_keyedChanges.begin(); it != m_keyedChanges.end(); ++it)
        {
            auto max = *std::max_element(
                        it->second.begin(),
                        it->second.end(),
                        [](CacheChange_t* c1, CacheChange_t* c2){ return c1->sourceTimestamp < c2->sourceTimestamp; });
            samples[num_samples++] = max;
        }
    }
}
