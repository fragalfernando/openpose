#include <iostream>
#include <openpose/experimental/tracking/pyramidalLK.hpp>
#include <openpose/experimental/tracking/personIdExtractor.hpp>

// #define LK_CUDA

namespace op
{
    float getEuclideanDistance(const cv::Point2f& a, const cv::Point2f& b)
    {
        try
        {
            const auto difference = a - b;
            return std::sqrt(difference.x * difference.x + difference.y * difference.y);
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
            return 0.f;
        }
    }

    std::vector<PersonEntry> captureKeypoints(const Array<float>& poseKeypoints, const float confidenceThreshold)
    {
        try
        {
            // Define result
            std::vector<PersonEntry> personEntries(poseKeypoints.getSize(0));
            // Fill personEntries
            for (auto p = 0; p < (int)personEntries.size(); p++)
            {
                // Create person entry in the tracking map
                auto& personEntry = personEntries[p];
                auto& keypoints = personEntry.keypoints;
                auto& status = personEntry.status;
                personEntry.counterLastDetection = 0;

                for (auto kp = 0; kp < poseKeypoints.getSize(1); kp++)
                {
                    cv::Point2f cp;
                    cp.x = poseKeypoints[{p,kp,0}];
                    cp.y = poseKeypoints[{p,kp,1}];
                    keypoints.emplace_back(cp);

                    if (poseKeypoints[{p,kp,2}] < confidenceThreshold)
                        status.emplace_back(1);
                    else
                        status.emplace_back(0);
                }
            }
            // Return result
            return personEntries;
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
            return {};
        }
    }

    void updateLK(std::unordered_map<int,PersonEntry>& personEntries, std::vector<cv::Mat>& pyramidImagesPrevious,
                  std::vector<cv::Mat>& pyramidImagesCurrent, const cv::Mat& imagePrevious,
                  const cv::Mat& imageCurrent, const int numberFramesToDeletePerson)
    {
        try
        {
            // Get all key values
            // Otherwise, `erase` provokes core dumped when removing elements
            std::vector<int> keyValues;
            keyValues.reserve(personEntries.size());
            for (const auto& entry : personEntries)
                keyValues.emplace_back(entry.first);
            // Update or remove elements
            for (auto& key : keyValues)
            {
                // const int idx = entry.first;
                auto& element = personEntries[key];

                // Remove keypoint
                if (element.counterLastDetection++ > numberFramesToDeletePerson)
                    personEntries.erase(key);
                // Update all keypoints for that entry
                else
                {
                    PersonEntry personEntry;
                    personEntry.counterLastDetection = element.counterLastDetection;
                    #ifdef LK_CUDA
                        UNUSED(pyramidImagesPrevious);
                        UNUSED(pyramidImagesCurrent);
                        pyramidalLKGpu(element.keypoints, personEntry.keypoints, element.status,
                                       imagePrevious, imageCurrent, 3, 21);
                    #else
                        pyramidalLKCpu(element.keypoints, personEntry.keypoints, pyramidImagesPrevious,
                                       pyramidImagesCurrent, element.status, imagePrevious, imageCurrent, 3, 21);
                    #endif
                    personEntry.status = element.status;
                    element = personEntry;
                }
            }
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
        }
    }

    void initializeLK(std::unordered_map<int,PersonEntry>& personEntries,
                     long long& mNextPersonId,
                     const Array<float>& poseKeypoints,
                     const float confidenceThreshold)
    {
        try
        {
            for (auto p = 0; p < poseKeypoints.getSize(0); p++)
            {
                const int currentPerson = mNextPersonId++;

                // Create person entry in the tracking map
                auto& personEntry = personEntries[currentPerson];
                auto& keypoints = personEntry.keypoints;
                auto& status = personEntry.status;
                personEntry.counterLastDetection = 0;

                for (auto kp = 0; kp < poseKeypoints.getSize(1); kp++)
                {
                    const cv::Point2f cp{poseKeypoints[{p,kp,0}], poseKeypoints[{p,kp,1}]};
                    keypoints.emplace_back(cp);

                    if (poseKeypoints[{p,kp,2}] < confidenceThreshold)
                        status.emplace_back(1);
                    else
                        status.emplace_back(0);
                }
            }
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
        }
    }

    Array<long long> matchLKAndOP(std::unordered_map<int,PersonEntry>& personEntries,
                                  long long& nextPersonId,
                                  const std::vector<PersonEntry>& openposePersonEntries,
                                  const float inlierRatioThreshold,
                                  const float distanceThreshold)
    {
        try
        {
            Array<long long> poseIds{(int)openposePersonEntries.size(), -1};
            std::unordered_map<int, PersonEntry> pendingQueue;

            if (!openposePersonEntries.empty())
            {
                const auto numberKeypoints = openposePersonEntries[0].keypoints.size();
                for (auto i = 0u; i < openposePersonEntries.size(); i++)
                {
                    auto bestMatch = -1ll;
                    auto bestScore = 0.f;

                    // Find best correspondance in the LK set
                    for (auto& personEntry : personEntries)
                    {
                        auto inliers = 0;
                        auto active = 0;
                        int idx = personEntry.first;

                        // Iterate through all keypoints
                        for (auto kp = 0u; kp < numberKeypoints; kp++)
                        {
                            // Not enough threshold
                            if (personEntries.at(idx).status.at(kp) || openposePersonEntries.at(i).status.at(kp))
                                continue;

                            active++;
                            const auto dist = getEuclideanDistance(personEntries.at(idx).keypoints.at(kp),
                                                                   openposePersonEntries.at(i).keypoints.at(kp));
                            // std::cout<<dist<<std::endl;
                            if (dist < distanceThreshold)
                                inliers ++;
                        }

                        float score = 0.f;

                        if (active)
                            score = inliers / (float)active;

                        //std::cout<<inliers<<std::endl;

                        if (score >= inlierRatioThreshold && score > bestScore)
                        {
                            bestScore = score;
                            bestMatch = personEntry.first;
                            // std::cout<<"BEST MATCH ENCOUNTERED"<<std::endl;
                        }
                    }
                    // Found a best match, update LK table and poseIds
                    if (bestMatch != -1)
                        poseIds.at(i) = bestMatch;
                    else
                        poseIds.at(i) = nextPersonId++;

                    pendingQueue[poseIds.at(i)] = openposePersonEntries.at(i);
                }
            }

            // Update LK table with pending queue
            for (auto& pendingQueueEntry: pendingQueue)
                personEntries[pendingQueueEntry.first] = pendingQueueEntry.second;

            return poseIds;
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
            return Array<long long>{};
        }
    }

    PersonIdExtractor::PersonIdExtractor(const float confidenceThreshold, const float inlierRatioThreshold,
                                         const float distanceThreshold, const int numberFramesToDeletePerson) :
        mConfidenceThreshold{confidenceThreshold},
        mInlierRatioThreshold{inlierRatioThreshold},
        mDistanceThreshold{distanceThreshold},
        mNumberFramesToDeletePerson{numberFramesToDeletePerson},
        mNextPersonId{0ll}
    {
    }

    PersonIdExtractor::~PersonIdExtractor()
    {
    }

    Array<long long> PersonIdExtractor::extractIds(const Array<float>& poseKeypoints, const cv::Mat& cvMatInput)
    {
        try
        {
            Array<long long> poseIds;
            const auto openposePersonEntries = captureKeypoints(poseKeypoints, mConfidenceThreshold);
// log(mPersonEntries.size());

            // First frame
            if (mImagePrevious.empty())
            {
                // Add first persons to the LK set
                initializeLK(mPersonEntries, mNextPersonId, poseKeypoints, mConfidenceThreshold);

                // Capture current frame as floating point
                cvMatInput.convertTo(mImagePrevious, CV_32F);
            }
            // Rest
            else
            {
                cv::Mat imageCurrent;
                std::vector<cv::Mat> pyramidImagesCurrent;
                cvMatInput.convertTo(imageCurrent, CV_32F);
                updateLK(mPersonEntries, mPyramidImagesPrevious, pyramidImagesCurrent, mImagePrevious, imageCurrent,
                         mNumberFramesToDeletePerson);
                mImagePrevious = imageCurrent;
                mPyramidImagesPrevious = pyramidImagesCurrent;
            }

            // Get poseIds and update LKset according to OpenPose set
            poseIds = matchLKAndOP(mPersonEntries, mNextPersonId, openposePersonEntries, mInlierRatioThreshold,
                                   mDistanceThreshold);

            return poseIds;
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
            return Array<long long>{};
        }
    }
}
