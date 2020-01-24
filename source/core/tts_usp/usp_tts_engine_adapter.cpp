//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// usp_tts_engine_adapter.cpp: Implementation definitions for CSpxMockTtsEngineAdapter C++ class
//

#include "stdafx.h"
#include "synthesis_helper.h"
#include "usp_tts_engine_adapter.h"
#include "create_object_helpers.h"
#include "guid_utils.h"
#include "handle_table.h"
#include "service_helpers.h"
#include "property_bag_impl.h"
#include "property_id_2_name_map.h"
#include "json.h"
#include "spx_build_information.h"
#include "time_utils.h"
#include "thread_service.h"

#define SPX_DBG_TRACE_USP_TTS 0

#define MAX_RETRY 1

#define WAIT_FOR_FIRST_CHUNK_TIMEOUT 10000  // in milliseconds
#define RECEIVE_ALL_CHUNKS_TIMEOUT 300000   // in milliseconds
#ifdef _DEBUG
#define DEBUG_WAIT_INTERVAL 100         // in milliseconds
#endif

namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {


CSpxUspTtsEngineAdapter::CSpxUspTtsEngineAdapter()
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
}

CSpxUspTtsEngineAdapter::~CSpxUspTtsEngineAdapter()
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
    SPX_DBG_TRACE_VERBOSE("%s: this=0x%8p", __FUNCTION__, (void*)this);
    SPX_DBG_ASSERT(m_uspCallbacks == nullptr);
    SPX_DBG_ASSERT(m_uspConnection == nullptr);
}

void CSpxUspTtsEngineAdapter::Init()
{
    // Initialize thread service
    m_threadService = SpxCreateObjectWithSite<ISpxThreadService>("CSpxThreadService", SpxQueryInterface<ISpxGenericSite>(GetSite()));

    // Get proxy setting
    GetProxySetting();

    // Initialize websocket platform
    Microsoft::CognitiveServices::Speech::USP::PlatformInit(m_proxyHost.data(), m_proxyPort, m_proxyUsername.data(), m_proxyPassword.data());

    // Initialize authentication related information

    std::string endpointUrl = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Endpoint), "");
    std::string hostUrl = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Host), "");
    std::string endpoint;
    std::string region = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Region), "");
    std::string subscriptionKey = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Key), "");

    if (!endpointUrl.empty()) // use custom endpoint
    {
        endpoint = endpointUrl;
    }
    else if (!hostUrl.empty()) // or custom host
    {
        endpoint = hostUrl;    // parse further in USP ConstructConnectionUrl()
        m_isCustomHost = true;
    }

    if (!endpoint.empty() && !CSpxSynthesisHelper::IsCustomVoiceEndpoint(endpoint) && !CSpxSynthesisHelper::IsStandardVoiceEndpoint(endpoint))
    {
        // Scenario 1, custom endpoint (e.g. on prem), no need authentication
        m_endpoint = endpoint;
    }
    else if (!endpoint.empty() && CSpxSynthesisHelper::IsCustomVoiceEndpoint(endpoint))
    {
        // Scenario 2, custom voice, need authentication (and therefore need initialize m_authenticator)
        m_endpoint = endpoint;
        region = CSpxSynthesisHelper::ParseRegionFromCognitiveServiceEndpoint(endpoint);

        // Construct cognitive service token issue URL based on region
        auto issueTokenUrl = std::string(HTTPS_URL_PREFIX) + region + ISSUE_TOKEN_HOST_SUFFIX + ISSUE_TOKEN_URL_PATH;
        if (!subscriptionKey.empty())
        {
            m_authenticator = std::make_shared<CSpxRestTtsAuthenticator>(issueTokenUrl, subscriptionKey, m_proxyHost, m_proxyPort, m_proxyUsername, m_proxyPassword);
        }
    }
    else if ((endpoint.empty() && !region.empty()) || (!endpoint.empty() && CSpxSynthesisHelper::IsStandardVoiceEndpoint(endpoint)))
    {
        // Scenario 3, standard voice, need issue token (and therefore need initialize m_authenticator)
        if (endpoint.empty() && !region.empty())
        {
            // Construct standard voice endpoint based on region
            m_endpoint = std::string(WSS_URL_PREFIX) + region + TTS_COGNITIVE_SERVICE_HOST_SUFFIX + TTS_COGNITIVE_SERVICE_WSS_URL_PATH;
        }

        if (!endpoint.empty() && CSpxSynthesisHelper::IsStandardVoiceEndpoint(endpoint))
        {
            m_endpoint = endpoint;
            region = CSpxSynthesisHelper::ParseRegionFromCognitiveServiceEndpoint(endpoint);
        }

        // Construct cognitive service token issue URL based on region
        auto issueTokenUrl = std::string(HTTPS_URL_PREFIX) + region + ISSUE_TOKEN_HOST_SUFFIX + ISSUE_TOKEN_URL_PATH;

        if (!subscriptionKey.empty())
        {
            m_authenticator = std::make_shared<CSpxRestTtsAuthenticator>(issueTokenUrl, subscriptionKey, m_proxyHost, m_proxyPort, m_proxyUsername, m_proxyPassword);
        }
    }
    else
    {
        ThrowRuntimeError("Invalid combination of endpoint, region and(or) subscription key.");
    }
}

void CSpxUspTtsEngineAdapter::Term()
{
    UspTerminate();
    SpxTerm(m_threadService);
}

void CSpxUspTtsEngineAdapter::SetOutput(std::shared_ptr<ISpxAudioOutput> output)
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
    m_audioOutput = output;
}

std::shared_ptr<ISpxSynthesisResult> CSpxUspTtsEngineAdapter::Speak(const std::string& text, bool isSsml, const std::wstring& requestId)
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
    SPX_DBG_ASSERT(UspState::Idle == m_uspState || UspState::Error == m_uspState);

    auto currentRequestId = requestId;
    std::shared_ptr<ISpxSynthesisResult> result;
    const auto maxRetry = std::stoi(ISpxPropertyBagImpl::GetStringValue("SpeechSynthesis_MaxRetryTimes", std::to_string(MAX_RETRY).c_str()));
    for (auto tryCount = 0; tryCount <= maxRetry; ++tryCount)
    {
        result = SpeakInternal(text, isSsml, currentRequestId);
        if (ResultReason::SynthesizingAudioCompleted == result->GetReason())
        {
            break;
        }
        else if (ResultReason::Canceled == result->GetReason() && !result->GetAudioData()->empty())
        {
            LogError("Synthesis cancelled with partial data received, cannot retry.");
            break;
        }

        currentRequestId = PAL::CreateGuidWithoutDashes();
        LogError("Synthesis cancelled without data received, retrying.");
    }

    return result;
}

std::shared_ptr<ISpxSynthesisResult> CSpxUspTtsEngineAdapter::SpeakInternal(const std::string& text, bool isSsml, const std::wstring& requestId)
{
    auto ssml = text;
    if (!isSsml)
    {
        auto language = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_SynthLanguage), "");
        auto voice = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_SynthVoice), "");
        ssml = CSpxSynthesisHelper::BuildSsml(text, language, voice);
    }

    SPX_DBG_TRACE_VERBOSE("SSML sent to TTS cognitive service: %s", ssml.data());

    EnsureUspConnection();

    if (UspState::Error != m_uspState)
    {
        // Set initial values for current utterance
        m_currentRequestId = requestId;
        m_currentText = PAL::ToWString(text);
        m_currentTextIsSsml = isSsml;
        m_currentTextOffset = 0;
        m_currentErrorCode = static_cast<USP::ErrorCode>(0);
        m_currentErrorMessage = std::string();

        // Send request
        m_uspState = UspState::Sending;
        m_currentReceivedData.clear();
        UspSendSynthesisContext(PAL::ToString(requestId));
        UspSendSsml(ssml, PAL::ToString(requestId));
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    // wait to get the first audio chunk
    const auto firstChunkTimeout = std::stoi(ISpxPropertyBagImpl::GetStringValue("SpeechSynthesis_FirstChunkTimeoutMs", std::to_string(WAIT_FOR_FIRST_CHUNK_TIMEOUT).c_str()));
#ifdef _DEBUG
    auto remainingTime = firstChunkTimeout;
    while (!m_cv.wait_for(lock, std::chrono::milliseconds(DEBUG_WAIT_INTERVAL), [&] { return m_uspState == UspState::ReceivingData || m_uspState == UspState::Idle || m_uspState == UspState::Error; }) && remainingTime > 0)
    {
        SPX_DBG_TRACE_VERBOSE("%s: waiting for USP to get first audio chunk ...", __FUNCTION__);
        remainingTime -= DEBUG_WAIT_INTERVAL;
    }
#else
    m_cv.wait_for(lock, std::chrono::milliseconds(firstChunkTimeout), [&] { return m_uspState == UspState::ReceivingData || m_uspState == UspState::Idle || m_uspState == UspState::Error; });
#endif

    if (m_uspState == UspState::Sending || m_uspState == UspState::TurnStarted)
    {
        LogError("USP error, timeout to get the first audio chunk.");
        m_currentErrorMessage = "USP error, timeout to get the first audio chunk.";
        m_uspState = UspState::Error;
        UspTerminate();
    }

    // wait to receive all audio data
    const auto allChunkTimeout = std::stoi(ISpxPropertyBagImpl::GetStringValue("SpeechSynthesis_AllChunkTimeoutMs", std::to_string(RECEIVE_ALL_CHUNKS_TIMEOUT).c_str()));
#ifdef _DEBUG
    remainingTime = allChunkTimeout;
    while (!m_cv.wait_for(lock, std::chrono::milliseconds(DEBUG_WAIT_INTERVAL), [&] { return m_uspState == UspState::Idle || m_uspState == UspState::Error; }) && remainingTime > 0)
    {
        SPX_DBG_TRACE_VERBOSE("%s: waiting for USP to finish receiving data ...", __FUNCTION__);
        remainingTime -= DEBUG_WAIT_INTERVAL;
    }
#else
    m_cv.wait_for(lock, std::chrono::milliseconds(allChunkTimeout), [&] { return m_uspState == UspState::Idle || m_uspState == UspState::Error; });
#endif

    if (m_uspState == UspState::ReceivingData)
    {
        LogError("USP error, timeout to get all audio data.");
        m_currentErrorMessage = "USP error, timeout to get all audio data.";
        m_currentErrorMessage += " Received audio size: " + CSpxSynthesisHelper::itos(m_currentReceivedData.size()) + "bytes.";
        m_uspState = UspState::Error;
        UspTerminate();
    }

    bool hasHeader = false;
    auto outputFormat = GetOutputFormat(m_audioOutput, &hasHeader);

    auto result = SpxCreateObjectWithSite<ISpxSynthesisResult>("CSpxSynthesisResult", SpxQueryInterface<ISpxGenericSite>(GetSite()));
    auto resultInit = SpxQueryInterface<ISpxSynthesisResultInit>(result);
    if (m_uspState == UspState::Error)
    {
        resultInit->InitSynthesisResult(requestId, ResultReason::Canceled,
            CancellationReason::Error, UspErrorCodeToCancellationErrorCode(m_currentErrorCode),
            m_currentReceivedData.data(), m_currentReceivedData.size(), outputFormat.get(), hasHeader);
        SpxQueryInterface<ISpxNamedProperties>(resultInit)->SetStringValue(GetPropertyName(PropertyId::CancellationDetails_ReasonDetailedText), m_currentErrorMessage.data());
    }
    else
    {
        resultInit->InitSynthesisResult(requestId, ResultReason::SynthesizingAudioCompleted,
            REASON_CANCELED_NONE, CancellationErrorCode::NoError, m_currentReceivedData.data(), m_currentReceivedData.size(), outputFormat.get(), hasHeader);
    }

    return result;
}

std::shared_ptr<ISpxNamedProperties> CSpxUspTtsEngineAdapter::GetParentProperties() const
{
    return SpxQueryService<ISpxNamedProperties>(GetSite());
}

void CSpxUspTtsEngineAdapter::GetProxySetting()
{
    m_proxyHost = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyHostName), "");
    m_proxyPort = std::stoi(ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyPort), "0"));
    if (m_proxyPort < 0)
    {
        ThrowInvalidArgumentException("Invalid proxy port: %d", m_proxyPort);
    }

    m_proxyUsername = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyUserName), "");
    m_proxyPassword = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyPassword), "");
}

void CSpxUspTtsEngineAdapter::SetSpeechConfigMessage()
{
    constexpr auto systemName = "SpeechSDK";

    nlohmann::json speechConfig;

    // Set system configuration data.
    speechConfig["context"]["system"]["version"] = BuildInformation::g_fullVersion;
    speechConfig["context"]["system"]["name"] = systemName;
    speechConfig["context"]["system"]["build"] = BuildInformation::g_buildPlatform;

    // Set OS configuration data.
    auto osInfo = PAL::getOperatingSystem();
    speechConfig["context"]["os"]["platform"] = osInfo.platform;
    speechConfig["context"]["os"]["name"] = osInfo.name;
    speechConfig["context"]["os"]["version"] = osInfo.version;

    m_speechConfig = speechConfig.dump();
}

void CSpxUspTtsEngineAdapter::UspSendSpeechConfig()
{
    constexpr auto messagePath = "speech.config";
    SPX_DBG_TRACE_VERBOSE("%s %s", messagePath, m_speechConfig.c_str());
    UspSendMessage(messagePath, m_speechConfig, USP::MessageType::Config);
}

void CSpxUspTtsEngineAdapter::UspSendSynthesisContext(const std::string& requestId)
{
    constexpr auto messagePath = "synthesis.context";

    // Set synthesis context data.
    nlohmann::json synthesisContext;
    synthesisContext["synthesis"]["audio"]["outputFormat"] = GetOutputFormatString(m_audioOutput);
    synthesisContext["synthesis"]["audio"]["metadataOptions"]["wordBoundaryEnabled"] = PAL::BoolToString(WordBoundaryEnabled());
    synthesisContext["synthesis"]["audio"]["metadataOptions"]["sentenceBoundaryEnabled"] = ISpxPropertyBagImpl::GetStringValue("SpeechServiceResponse_Synthesis_SentenceBoundaryEnabled", "false");

    UspSendMessage(messagePath, synthesisContext.dump(), USP::MessageType::Context, requestId);
}

void CSpxUspTtsEngineAdapter::UspSendSsml(const std::string& ssml, const std::string& requestId)
{
    constexpr auto messagePath = "ssml";
    SPX_DBG_TRACE_VERBOSE("%s %s", messagePath, ssml.c_str());
    UspSendMessage(messagePath, ssml, USP::MessageType::Ssml, requestId);
}

void CSpxUspTtsEngineAdapter::UspSendMessage(const std::string& messagePath, const std::string &buffer, USP::MessageType messageType, const std::string& requestId)
{
    SPX_DBG_TRACE_VERBOSE("%s='%s'", messagePath.c_str(), buffer.c_str());
    std::packaged_task<void()> task([=]() { DoSendMessageWork(m_uspConnection, messagePath, buffer, messageType, requestId); });
    m_threadService->ExecuteAsync(move(task));
}

void CSpxUspTtsEngineAdapter::DoSendMessageWork(std::weak_ptr<USP::Connection> connectionPtr, const std::string& messagePath, const std::string& buffer, USP::MessageType messageType, const std::string& requestId)
{
    auto connection = connectionPtr.lock();
    SPX_DBG_ASSERT(connection != nullptr);
    if (connection != nullptr)
    {
        connection->SendMessage(messagePath, reinterpret_cast<const uint8_t*>(buffer.c_str()), buffer.length(), messageType, requestId);
    }
}

void CSpxUspTtsEngineAdapter::EnsureUspConnection()
{
    m_uspState = UspState::Connecting;
    if (m_uspConnection == nullptr)
    {
        UspInitialize();
    }
    else if (!m_uspConnection->IsConnected())
    {
        // If the connection was closed due to any reason, we re-connect
        UspTerminate();
        UspInitialize();
    }
    else if (PAL::GetTicks(std::chrono::system_clock::now() - m_lastConnectTime) > static_cast<uint64_t>(9) * 60 * 1000 * 10000)
    {
        // Per https://speechwiki.azurewebsites.net/partners/protocol-websockets-tts.html#connection-duration-limitations
        // The service closes the active connect after 10 mins.
        // We re-connect it after 9 mins in case it breaks an on-going speak.
        UspTerminate();
        UspInitialize();
    }
}

void CSpxUspTtsEngineAdapter::UspInitialize()
{
    SPX_DBG_TRACE_VERBOSE("%s: this=0x%8p", __FUNCTION__, (void*)this);
    SPX_IFTRUE_THROW_HR(m_uspConnection != nullptr, SPXERR_ALREADY_INITIALIZED);

    // Fill authorization token
    std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
    std::string token;
    if (m_authenticator != nullptr)
    {
        const auto getAccessTokenTimeout = std::stoi(ISpxPropertyBagImpl::GetStringValue("SpeechSynthesis_GetAccessTokenTimeoutMs", "5000"));
        token = m_authenticator->GetAccessToken(getAccessTokenTimeout);
    }

    if (token.empty())
    {
        token = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceAuthorization_Token), "");
    }

    if (token.empty())
    {
        SPX_TRACE_ERROR("Error: issue token is empty, check your subscription key and network.");
        m_currentErrorMessage = "Error occurs while getting access token, check your subscription key, access token or network connection.";
        m_uspState = UspState::Error;
        return;
    }
    else
    {
        authData[static_cast<size_t>(USP::AuthenticationType::AuthorizationToken)] = token;
    }

    // Create the usp client, which we'll configure and use to create the actual connection
    auto uspCallbacks = SpxCreateObjectWithSite<ISpxUspCallbacks>("CSpxUspCallbackWrapper", this);
    auto client = USP::Client(uspCallbacks, USP::EndpointType::SpeechSynthesis, PAL::CreateGuidWithoutDashes(), m_threadService)
        .SetAuthentication(authData);

    if (m_isCustomHost) // for FromHost specific handling
    {
        client.SetHostUrl(m_endpoint);
    }
    else
    {
        client.SetEndpointUrl(m_endpoint);
    }

    // Set proxy
    if (!m_proxyHost.empty() && m_proxyPort > 0)
    {
        client.SetProxyServerInfo(m_proxyHost.data(), m_proxyPort, m_proxyUsername.data(), m_proxyPassword.data());
    }

    // Try to connect
    USP::ConnectionPtr uspConnection;

    try
    {
        uspConnection = client.Connect();
    }
    catch (const std::exception& e)
    {
        SPX_TRACE_ERROR("Error: '%s'", e.what());
        OnError(true, USP::ErrorCode::ConnectionError, e.what());
    }
    catch (...)
    {
        SPX_TRACE_ERROR("Error: Unexpected exception in UspInitialize");
        OnError(true, USP::ErrorCode::ConnectionError, "Error: Unexpected exception in UspInitialize");
    }

    // if error occurs in the above client.Connect, set state to error and return.
    if (uspConnection == nullptr)
    {
        m_uspState = UspState::Error;
        m_currentErrorMessage = "USP connection establishment failed.";
        return;
    }

    // We're done!!
    m_uspCallbacks = uspCallbacks;
    m_uspConnection = std::move(uspConnection);
    m_uspState = UspState::Idle;
    m_lastConnectTime = std::chrono::system_clock::now();

    // Send speech config message
    if (m_uspConnection != nullptr)
    {
        ISpxPropertyBagImpl::SetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Url), m_uspConnection->GetConnectionUrl().c_str());

        // Construct config message payload
        SetSpeechConfigMessage();

        // Send speech config
        UspSendSpeechConfig();
    }
}

void CSpxUspTtsEngineAdapter::UspTerminate()
{
    // Term the callbacks first and then reset/release the connection
    SpxTermAndClear(m_uspCallbacks);

    m_uspConnection.reset();

}

void CSpxUspTtsEngineAdapter::OnTurnStart(const USP::TurnStartMsg& message)
{
    UNUSED(message);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_uspState = UspState::TurnStarted;
    m_currentReceivedData.clear();
    m_cv.notify_all();
}

void CSpxUspTtsEngineAdapter::OnAudioOutputChunk(const USP::AudioOutputChunkMsg& message)
{
    InvokeOnSite([this, message](const SitePtr& p) {
        if (message.audioLength > 0)
        {
            p->Write(this, m_currentRequestId, (uint8_t *)message.audioBuffer, (uint32_t)message.audioLength);
        }
    });

    std::unique_lock<std::mutex> lock(m_mutex);
    m_uspState = UspState::ReceivingData;
    auto originalSize = m_currentReceivedData.size();
    m_currentReceivedData.resize(originalSize + message.audioLength);
    memcpy(m_currentReceivedData.data() + originalSize, message.audioBuffer, message.audioLength);
    m_cv.notify_all();
}

void CSpxUspTtsEngineAdapter::OnAudioOutputMetadata(const USP::AudioOutputMetadataMsg& message)
{
    auto synthesizerEvents = SpxQueryInterface<ISpxSynthesizerEvents>(GetSite());

    for (auto iterator = message.metadatas.begin(); iterator != message.metadatas.end(); ++iterator)
    {
        if (PAL::stricmp(iterator->type.data(), METADATA_TYPE_WORD_BOUNDARY) == 0)
        {
            auto wordBoundary = iterator->textBoundary;
            auto textOffset = m_currentText.find(wordBoundary.text, m_currentTextOffset);

            if (m_currentTextIsSsml)
            {
                while (textOffset != std::string::npos && InSsmlTag(textOffset, m_currentText, m_currentTextOffset))
                {
                    textOffset = m_currentText.find(wordBoundary.text, textOffset + wordBoundary.text.length());
                }
            }

            if (textOffset != std::string::npos)
            {
                m_currentTextOffset = static_cast<uint32_t>(textOffset + wordBoundary.text.length());
                synthesizerEvents->FireWordBoundary(wordBoundary.audioOffset, static_cast<uint32_t>(textOffset), static_cast<uint32_t>(wordBoundary.text.length()));
            }
        }
    }
}

void CSpxUspTtsEngineAdapter::OnTurnEnd(const USP::TurnEndMsg& message)
{
    UNUSED(message);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_uspState = UspState::Idle;
    m_cv.notify_all();
}

void CSpxUspTtsEngineAdapter::OnError(bool transport, USP::ErrorCode errorCode, const std::string& errorMessage)
{
    UNUSED(transport);
    SPX_DBG_TRACE_VERBOSE("Response: On Error: Code:%d, Message: %s.\n", errorCode, errorMessage.c_str());
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_uspState != UspState::Idle) // no need to raise an error if synthesis is already done.
    {
        m_currentErrorCode = errorCode;
        m_currentErrorMessage = errorMessage;
        m_currentErrorMessage += " USP state: " + CSpxSynthesisHelper::itos((int)(UspState)m_uspState) + ".";
        m_currentErrorMessage += " Received audio size: " + CSpxSynthesisHelper::itos(m_currentReceivedData.size()) + "bytes.";
        m_uspState = UspState::Error;
        m_cv.notify_all();
    }

    // terminate usp connection on error
    UspTerminate();
}

SpxWAVEFORMATEX_Type CSpxUspTtsEngineAdapter::GetOutputFormat(std::shared_ptr<ISpxAudioOutput> output, bool* hasHeader)
{
    auto audioStream = SpxQueryInterface<ISpxAudioStream>(output);
    auto requiredFormatSize = audioStream->GetFormat(nullptr, 0);
    auto format = SpxAllocWAVEFORMATEX(requiredFormatSize);
    audioStream->GetFormat(format.get(), requiredFormatSize);

    if (hasHeader != nullptr)
    {
        *hasHeader = SpxQueryInterface<ISpxAudioOutputFormat>(output)->HasHeader();
    }

    return format;
}

std::string CSpxUspTtsEngineAdapter::GetOutputFormatString(std::shared_ptr<ISpxAudioOutput> output)
{
    auto outputFormat = SpxQueryInterface<ISpxAudioOutputFormat>(output);
    auto formatString = outputFormat->GetFormatString();
    if (outputFormat->HasHeader())
    {
        formatString = outputFormat->GetRawFormatString();
    }

    return formatString;
}

bool CSpxUspTtsEngineAdapter::WordBoundaryEnabled() const
{
    auto wordBoundaryEnabled = PAL::ToBool(ISpxPropertyBagImpl::GetStringValue("SpeechServiceResponse_Synthesis_WordBoundaryEnabled", "true"));

    auto synthesizerEvents = SpxQueryInterface<ISpxSynthesizerEvents>(GetSite());
    auto wordBoundaryConnected = synthesizerEvents->WordBoundary.IsConnected();

    return wordBoundaryEnabled && wordBoundaryConnected;
}

bool CSpxUspTtsEngineAdapter::InSsmlTag(size_t currentPos, const std::wstring& ssml, size_t beginningPos)
{
    if (currentPos < beginningPos || currentPos >= ssml.length() || beginningPos >= ssml.length())
    {
        return false;
    }

    auto pos = currentPos;
    while (pos >= beginningPos)
    {
        if (*(ssml.data() + pos) == '>')
        {
            return false;
        }

        if (*(ssml.data() + pos) == '<')
        {
            return true;
        }

        pos--;
    }

    return false;
}

CancellationErrorCode CSpxUspTtsEngineAdapter::UspErrorCodeToCancellationErrorCode(USP::ErrorCode uspErrorCode)
{
    std::map<USP::ErrorCode, CancellationErrorCode> uspErrorCodeToCancellationErrorCodeMapping = {
        { USP::ErrorCode::AuthenticationError, CancellationErrorCode::AuthenticationFailure },
        { USP::ErrorCode::BadRequest, CancellationErrorCode::BadRequest },
        { USP::ErrorCode::ConnectionError, CancellationErrorCode::ConnectionFailure },
        { USP::ErrorCode::Forbidden, CancellationErrorCode::Forbidden },
        { USP::ErrorCode::RuntimeError, CancellationErrorCode::RuntimeError },
        { USP::ErrorCode::ServiceError, CancellationErrorCode::ServiceError },
        { USP::ErrorCode::ServiceUnavailable, CancellationErrorCode::ServiceUnavailable },
        { USP::ErrorCode::TooManyRequests, CancellationErrorCode::TooManyRequests }
    };

    auto cancellationErrorCode = CancellationErrorCode::NoError;

    auto iter = uspErrorCodeToCancellationErrorCodeMapping.find(uspErrorCode);
    if (iter != uspErrorCodeToCancellationErrorCodeMapping.end())
    {
        cancellationErrorCode = iter->second;
    }

    return cancellationErrorCode;
}


} } } } // Microsoft::CognitiveServices::Speech::Impl
