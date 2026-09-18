#pragma once
#include <IFovAlg.h>
#include <vector>
#include <string>
#include <array>
#include <opencv2/opencv.hpp>
#include <Mil.h>

enum class eDepthPreprocType
{
    INNERCENTER,
    BEAD,
    INSHOULDER,
    UNKNOWN
};

//------------------------------------------------------------
// C3DPreprocess
//------------------------------------------------------------
class C3DPreprocess : public AIV::IFovAlg
{
public:
    // IFovAlg 인터페이스
    void Initial() override;
    void SetParam(const wchar_t* szParamIniPath, int* pParams, size_t nParamLen) override;
    int  Final() override;
    int  Reset(const char* szInnerID, const wchar_t* szProductID) override;
    int  SetInputImg(int nFovIdx, MIL_ID milInputImg) override;
    int  SetOutputImg(int nFovIdx, MIL_ID milOutputImg) override;
    int  INSPECT(UINT_PTR dwInspThreadID, UINT nMsgType) override;

private:
    void ReadInfo(std::shared_ptr<AiV::Utils::IIniReaderManager> pReader);
    void ReadSection(std::shared_ptr<AiV::Utils::IIniSectionData> pSection);

    // 전처리
    void IncenterProcessing(const cv::Mat& src16u, cv::Mat& out8u);
    void InShoulderProcessing(const cv::Mat& src16u, cv::Mat& out8u);

    cv::Mat DeepCopyMil2OpencvNormalize(MIL_ID milImage);

    void CopyOpencv2Mil_KeepBuffer_If32(const cv::Mat& cv8u, MIL_ID milOutput);

private:

    std::wstring          m_strName;
    eDepthPreprocType     m_eDepthType;
    std::array<int, 4>    m_roi; // INSHOULDER용 ROI

    cv::Size			  m_PatchSize{ 15,15 };

    double				  m_lowerPct, m_upper_pct, m_overlap;

    UINT_PTR              m_dwInspThreadID;
    UINT                  m_nMsgType;
    int                   m_nOutputImgIdx;
    std::vector<int>      m_vecIniIdx;


    std::vector<MIL_ID>   m_vecInput;
    std::vector<MIL_ID>   m_vecOutput;
    MIL_ID                m_milTempMax;
    int m_nStagePos = 0;
};
