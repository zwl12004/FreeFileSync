// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "gui_status_handler.h"
#include <zen/shell_execute.h>
#include <zen/shutdown.h>
#include <wx/app.h>
#include <wx/wupdlock.h>
#include <wx+/popup_dlg.h>
#include "main_dlg.h"
#include "../base/log_file.h"
#include "../base/resolve_path.h"
#include "../afs/concrete.h"

using namespace zen;
using namespace fff;


StatusHandlerTemporaryPanel::StatusHandlerTemporaryPanel(MainDialog& dlg,
                                                         const std::chrono::system_clock::time_point& startTime,
                                                         bool ignoreErrors,
                                                         size_t automaticRetryCount,
                                                         std::chrono::seconds automaticRetryDelay) :
    mainDlg_(dlg),
    automaticRetryCount_(automaticRetryCount),
    automaticRetryDelay_(automaticRetryDelay),
    startTime_(startTime)
{
    {
        mainDlg_.compareStatus_->init(*this, ignoreErrors, automaticRetryCount); //clear old values before showing panel

        //------------------------------------------------------------------
        const wxAuiPaneInfo& topPanel = mainDlg_.auiMgr_.GetPane(mainDlg_.m_panelTopButtons);
        wxAuiPaneInfo& statusPanel    = mainDlg_.auiMgr_.GetPane(mainDlg_.compareStatus_->getAsWindow());

        //determine best status panel row near top panel
        switch (topPanel.dock_direction)
        {
            case wxAUI_DOCK_TOP:
            case wxAUI_DOCK_BOTTOM:
                statusPanel.Layer    (topPanel.dock_layer);
                statusPanel.Direction(topPanel.dock_direction);
                statusPanel.Row      (topPanel.dock_row + 1);
                break;

            case wxAUI_DOCK_LEFT:
            case wxAUI_DOCK_RIGHT:
                statusPanel.Layer    (std::max(0, topPanel.dock_layer - 1));
                statusPanel.Direction(wxAUI_DOCK_TOP);
                statusPanel.Row      (0);
                break;
                //case wxAUI_DOCK_CENTRE:
        }

        wxAuiPaneInfoArray& paneArray = mainDlg_.auiMgr_.GetAllPanes();

        const bool statusRowTaken = [&]
        {
            for (size_t i = 0; i < paneArray.size(); ++i)
            {
                const wxAuiPaneInfo& paneInfo = paneArray[i];
                //doesn't matter if paneInfo.IsShown() or not! => move down in either case!
                if (&paneInfo != &statusPanel &&
                    paneInfo.dock_layer     == statusPanel.dock_layer &&
                    paneInfo.dock_direction == statusPanel.dock_direction &&
                    paneInfo.dock_row       == statusPanel.dock_row)
                    return true;
            }
            return false;
        }();

        //move all rows that are in the way one step further
        if (statusRowTaken)
            for (size_t i = 0; i < paneArray.size(); ++i)
            {
                wxAuiPaneInfo& paneInfo = paneArray[i];

                if (&paneInfo != &statusPanel &&
                    paneInfo.dock_layer     == statusPanel.dock_layer &&
                    paneInfo.dock_direction == statusPanel.dock_direction &&
                    paneInfo.dock_row       >= statusPanel.dock_row)
                    ++paneInfo.dock_row;
            }
        //------------------------------------------------------------------

        statusPanel.Show();
        mainDlg_.auiMgr_.Update();
        mainDlg_.compareStatus_->getAsWindow()->Refresh(); //macOS: fix background corruption for the statistics boxes (call *after* wxAuiManager::Update()
    }

    mainDlg_.Update(); //don't wait until idle event!

    //register keys
    mainDlg_.Connect(wxEVT_CHAR_HOOK, wxKeyEventHandler(StatusHandlerTemporaryPanel::OnKeyPressed), nullptr, this);
    mainDlg_.m_buttonCancel->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusHandlerTemporaryPanel::OnAbortCompare), nullptr, this);
}


StatusHandlerTemporaryPanel::~StatusHandlerTemporaryPanel()
{
    //unregister keys
    mainDlg_.Disconnect(wxEVT_CHAR_HOOK, wxKeyEventHandler(StatusHandlerTemporaryPanel::OnKeyPressed), nullptr, this);
    mainDlg_.m_buttonCancel->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusHandlerTemporaryPanel::OnAbortCompare), nullptr, this);

    //Workaround wxAuiManager crash when starting panel resizing during comparison and holding button until after comparison has finished:
    //- unlike regular window resizing, wxAuiManager does not run a dedicated event loop while the mouse button is held
    //- wxAuiManager internally stores the panel index that is currently resized
    //- our previous hiding of the compare status panel invalidates this index
    // => the next mouse move will have wxAuiManager crash => another fine piece of "wxQuality" code
    // => mitigate:
    wxMouseCaptureLostEvent dummy;
    mainDlg_.auiMgr_.ProcessEvent(dummy); //should be no-op if no mouse buttons are pressed

    mainDlg_.auiMgr_.GetPane(mainDlg_.compareStatus_->getAsWindow()).Hide();
    mainDlg_.auiMgr_.Update();
    mainDlg_.compareStatus_->teardown();

    if (!errorLog_.empty()) //reportFinalStatus() was not called!
        std::abort();
}


StatusHandlerTemporaryPanel::Result StatusHandlerTemporaryPanel::reportFinalStatus() //noexcept!!
{
    const auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - startTime_);

    //determine post-sync status irrespective of further errors during tear-down
    const SyncResult finalStatus = [&]
    {
        if (getAbortStatus())
        {
            errorLog_.logMsg(_("Stopped"), MSG_TYPE_ERROR); //= user cancel; *not* a MSG_TYPE_FATAL_ERROR!
            return SyncResult::aborted;
        }
        else if (errorLog_.getItemCount(MSG_TYPE_ERROR | MSG_TYPE_FATAL_ERROR) > 0)
            return SyncResult::finishedError;
        else if (errorLog_.getItemCount(MSG_TYPE_WARNING) > 0)
            return SyncResult::finishedWarning;
        else
            return SyncResult::finishedSuccess;
    }();

    const ProcessSummary summary
    {
        startTime_, finalStatus, {} /*jobName*/,
        getStatsCurrent(currentPhase()),
        getStatsTotal  (currentPhase()),
        totalTime
    };

    auto errorLogFinal = std::make_shared<const ErrorLog>(std::move(errorLog_));
    errorLog_ = ErrorLog(); //see check in ~StatusHandlerTemporaryPanel()

    return { summary, errorLogFinal };
}


void StatusHandlerTemporaryPanel::initNewPhase(int itemsTotal, int64_t bytesTotal, Phase phaseID)
{
    StatusHandler::initNewPhase(itemsTotal, bytesTotal, phaseID);

    mainDlg_.compareStatus_->initNewPhase(); //call after "StatusHandler::initNewPhase"

    forceUiRefresh(); //throw AbortProcess; OS X needs a full yield to update GUI and get rid of "dummy" texts
}


void StatusHandlerTemporaryPanel::logInfo(const std::wstring& msg)
{
    errorLog_.logMsg(msg, MSG_TYPE_INFO);
}


void StatusHandlerTemporaryPanel::reportWarning(const std::wstring& msg, bool& warningActive)
{
    PauseTimers dummy(*mainDlg_.compareStatus_);

    errorLog_.logMsg(msg, MSG_TYPE_WARNING);

    if (!warningActive) //if errors are ignored, then warnings should also
        return;

    if (!mainDlg_.compareStatus_->getOptionIgnoreErrors())
    {
        forceUiRefreshNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

        bool dontWarnAgain = false;
        switch (showConfirmationDialog(&mainDlg_, DialogInfoType::warning,
                                       PopupDialogCfg().setDetailInstructions(msg).
                                       setCheckBox(dontWarnAgain, _("&Don't show this warning again")),
                                       _("&Ignore")))
        {
            case ConfirmationButton::accept:
                warningActive = !dontWarnAgain;
                break;
            case ConfirmationButton::cancel:
                userAbortProcessNow(); //throw AbortProcess
                break;
        }
    }
    //else: if errors are ignored, then warnings should also
}


ProcessCallback::Response StatusHandlerTemporaryPanel::reportError(const std::wstring& msg, size_t retryNumber)
{
    PauseTimers dummy(*mainDlg_.compareStatus_);

    //auto-retry
    if (retryNumber < automaticRetryCount_)
    {
        errorLog_.logMsg(msg + L"\n-> " + _("Automatic retry"), MSG_TYPE_INFO);
        delayAndCountDown(_("Automatic retry") + (automaticRetryCount_ <= 1 ? L"" :  L" " + numberTo<std::wstring>(retryNumber + 1) + L"/" + numberTo<std::wstring>(automaticRetryCount_)),
        automaticRetryDelay_, [&](const std::wstring& statusMsg) { this->reportStatus(_("Error") + L": " + statusMsg); }); //throw AbortProcess
        return ProcessCallback::retry;
    }

    //always, except for "retry":
    auto guardWriteLog = zen::makeGuard<ScopeGuardRunMode::ON_EXIT>([&] { errorLog_.logMsg(msg, MSG_TYPE_ERROR); });

    if (!mainDlg_.compareStatus_->getOptionIgnoreErrors())
    {
        forceUiRefreshNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

        switch (showConfirmationDialog(&mainDlg_, DialogInfoType::error,
                                       PopupDialogCfg().setDetailInstructions(msg),
                                       _("&Ignore"), _("Ignore &all"), _("&Retry")))
        {
            case ConfirmationButton3::accept: //ignore
                return ProcessCallback::ignoreError;

            case ConfirmationButton3::acceptAll: //ignore all
                mainDlg_.compareStatus_->setOptionIgnoreErrors(true);
                return ProcessCallback::ignoreError;

            case ConfirmationButton3::decline: //retry
                guardWriteLog.dismiss();
                errorLog_.logMsg(msg + L"\n-> " + _("Retrying operation..."), MSG_TYPE_INFO); //explain why there are duplicate "doing operation X" info messages in the log!
                return ProcessCallback::retry;

            case ConfirmationButton3::cancel:
                userAbortProcessNow(); //throw AbortProcess
                break;
        }
    }
    else
        return ProcessCallback::ignoreError;

    assert(false);
    return ProcessCallback::ignoreError; //dummy return value
}


void StatusHandlerTemporaryPanel::reportFatalError(const std::wstring& msg)
{
    PauseTimers dummy(*mainDlg_.compareStatus_);

    errorLog_.logMsg(msg, MSG_TYPE_FATAL_ERROR);

    if (!mainDlg_.compareStatus_->getOptionIgnoreErrors())
    {
        forceUiRefreshNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

        switch (showConfirmationDialog(&mainDlg_, DialogInfoType::error,
                                       PopupDialogCfg().setTitle(_("Serious Error")).
                                       setDetailInstructions(msg),
                                       _("&Ignore"), _("Ignore &all")))
        {
            case ConfirmationButton2::accept: //ignore
                break;

            case ConfirmationButton2::acceptAll: //ignore all
                mainDlg_.compareStatus_->setOptionIgnoreErrors(true);
                break;

            case ConfirmationButton2::cancel:
                userAbortProcessNow(); //throw AbortProcess
                break;
        }
    }
}


void StatusHandlerTemporaryPanel::forceUiRefreshNoThrow()
{
    mainDlg_.compareStatus_->updateGui();
}


void StatusHandlerTemporaryPanel::OnKeyPressed(wxKeyEvent& event)
{
    const int keyCode = event.GetKeyCode();
    if (keyCode == WXK_ESCAPE)
    {
        wxCommandEvent dummy;
        OnAbortCompare(dummy);
    }

    event.Skip();
}


void StatusHandlerTemporaryPanel::OnAbortCompare(wxCommandEvent& event)
{
    userRequestAbort();
}

//########################################################################################################

StatusHandlerFloatingDialog::StatusHandlerFloatingDialog(wxFrame* parentDlg,
                                                         const std::chrono::system_clock::time_point& startTime,
                                                         bool ignoreErrors,
                                                         size_t automaticRetryCount,
                                                         std::chrono::seconds automaticRetryDelay,
                                                         const std::wstring& jobName,
                                                         const Zstring& soundFileSyncComplete,
                                                         const Zstring& postSyncCommand,
                                                         PostSyncCondition postSyncCondition,
                                                         bool& autoCloseDialog) :
    progressDlg_(SyncProgressDialog::create([this] { userRequestAbort(); }, *this, parentDlg, true /*showProgress*/, autoCloseDialog,
startTime, jobName, soundFileSyncComplete, ignoreErrors, automaticRetryCount, PostSyncAction2::none)),
           automaticRetryCount_(automaticRetryCount),
           automaticRetryDelay_(automaticRetryDelay),
           jobName_(jobName),
           startTime_(startTime),
           postSyncCommand_(postSyncCommand),
           postSyncCondition_(postSyncCondition),
autoCloseDialogOut_(autoCloseDialog) {}


StatusHandlerFloatingDialog::~StatusHandlerFloatingDialog()
{
    if (progressDlg_) //reportFinalStatus() was not called!
        std::abort();
}


StatusHandlerFloatingDialog::Result StatusHandlerFloatingDialog::reportFinalStatus(const Zstring& altLogFolderPathPhrase, int logfilesMaxAgeDays, const std::set<AbstractPath>& logFilePathsToKeep)
{
    const auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - startTime_);

    progressDlg_->timerSetStatus(false /*active*/); //keep correct summary window stats considering count down timer, system sleep

    //determine post-sync status irrespective of further errors during tear-down
    const SyncResult finalStatus = [&]
    {
        if (getAbortStatus())
        {
            errorLog_.logMsg(_("Stopped"), MSG_TYPE_ERROR); //= user cancel; *not* a MSG_TYPE_FATAL_ERROR!
            return SyncResult::aborted;
        }
        else if (errorLog_.getItemCount(MSG_TYPE_ERROR | MSG_TYPE_FATAL_ERROR) > 0)
            return SyncResult::finishedError;
        else if (errorLog_.getItemCount(MSG_TYPE_WARNING) > 0)
            return SyncResult::finishedWarning;

        if (getStatsTotal(currentPhase()) == ProgressStats())
            errorLog_.logMsg(_("Nothing to synchronize"), MSG_TYPE_INFO);
        return SyncResult::finishedSuccess;
    }();

    assert(finalStatus == SyncResult::aborted || currentPhase() == PHASE_SYNCHRONIZING);

    const ProcessSummary summary
    {
        startTime_, finalStatus, jobName_,
        getStatsCurrent(currentPhase()),
        getStatsTotal  (currentPhase()),
        totalTime
    };

    //post sync command
    Zstring commandLine = [&]
    {
        if (getAbortStatus() && *getAbortStatus() == AbortTrigger::USER)
            ; //user cancelled => don't run post sync command!
        else
            switch (postSyncCondition_)
            {
                case PostSyncCondition::COMPLETION:
                    return postSyncCommand_;
                case PostSyncCondition::ERRORS:
                    if (finalStatus == SyncResult::aborted ||
                        finalStatus == SyncResult::finishedError)
                        return postSyncCommand_;
                    break;
                case PostSyncCondition::SUCCESS:
                    if (finalStatus == SyncResult::finishedWarning ||
                        finalStatus == SyncResult::finishedSuccess)
                        return postSyncCommand_;
                    break;
            }
        return Zstring();
    }();
    trim(commandLine);

    if (!commandLine.empty())
        errorLog_.logMsg(_("Executing command:") + L" " + utfTo<std::wstring>(commandLine), MSG_TYPE_INFO);

    //----------------- always save log under %appdata%\FreeFileSync\Logs ------------------------
    AbstractPath logFilePath = getNullPath();
    try
    {
        //do NOT use tryReportingError()! saving log files should not be cancellable!
        auto notifyStatusNoThrow = [&](const std::wstring& msg) { try { reportStatus(msg); /*throw AbortProcess*/ } catch (...) {} };
        logFilePath = saveLogFile(summary, errorLog_, altLogFolderPathPhrase, logfilesMaxAgeDays, logFilePathsToKeep, notifyStatusNoThrow); //throw FileError
    }
    catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }

    //execute post sync command *after* writing log files, so that user can refer to the log via the command!
    if (!commandLine.empty())
        try
        {
            //----------------------------------------------------------------------
            ::wxSetEnv(L"logfile_path", AFS::getDisplayPath(logFilePath));
            //----------------------------------------------------------------------
            //use ExecutionType::ASYNC until there is reason not to: https://freefilesync.org/forum/viewtopic.php?t=31
            shellExecute(expandMacros(commandLine), ExecutionType::ASYNC, false/*hideConsole*/); //throw FileError
        }
        catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }

    auto mayRunAfterCountDown = [&](const std::wstring& operationName)
    {
        auto notifyStatusThrowOnCancel = [&](const std::wstring& msg)
        {
            try { reportStatus(msg); /*throw AbortProcess*/ }
            catch (...)
            {
                if (getAbortStatus() && *getAbortStatus() == AbortTrigger::USER)
                    throw;
            }
        };

        if (progressDlg_->getWindowIfVisible())
            try
            {
                delayAndCountDown(operationName, std::chrono::seconds(5), notifyStatusThrowOnCancel); //throw AbortProcess
            }
            catch (...) { return false; }

        return true;
    };

    //post sync action
    bool autoClose = false;
    FinalRequest finalRequest = FinalRequest::none;

    if (getAbortStatus() && *getAbortStatus() == AbortTrigger::USER)
        ; //user cancelled => don't run post sync command!
    else
        switch (progressDlg_->getOptionPostSyncAction())
        {
            case PostSyncAction2::none:
                autoClose = progressDlg_->getOptionAutoCloseDialog();
                break;
            case PostSyncAction2::exit:
                autoClose = true;
                finalRequest = FinalRequest::exit; //program exit must be handled by calling context!
                break;
            case PostSyncAction2::sleep:
                if (mayRunAfterCountDown(_("System: Sleep")))
                    try
                    {
                        suspendSystem(); //throw FileError
                        autoClose = progressDlg_->getOptionAutoCloseDialog();
                    }
                    catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }
                break;
            case PostSyncAction2::shutdown:
                if (mayRunAfterCountDown(_("System: Shut down")))
                {
                    autoClose = true;
                    finalRequest = FinalRequest::shutdown; //system shutdown must be handled by calling context!
                }
                break;
        }

    auto errorLogFinal = std::make_shared<const ErrorLog>(std::move(errorLog_));

    autoCloseDialogOut_ = //output parameter owned by SyncProgressDialog (evaluate *after* user closed the results dialog)
        progressDlg_->destroy(autoClose,
                              finalRequest == FinalRequest::none /*restoreParentFrame*/,
                              finalStatus, errorLogFinal).autoCloseDialog;
    progressDlg_ = nullptr;

    return { summary, errorLogFinal, finalRequest, logFilePath };
}


void StatusHandlerFloatingDialog::initNewPhase(int itemsTotal, int64_t bytesTotal, Phase phaseID)
{
    assert(phaseID == PHASE_SYNCHRONIZING);
    StatusHandler::initNewPhase(itemsTotal, bytesTotal, phaseID);
    progressDlg_->initNewPhase(); //call after "StatusHandler::initNewPhase"

    forceUiRefresh(); //throw AbortProcess; OS X needs a full yield to update GUI and get rid of "dummy" texts
}


void StatusHandlerFloatingDialog::logInfo(const std::wstring& msg)
{
    errorLog_.logMsg(msg, MSG_TYPE_INFO);
}


void StatusHandlerFloatingDialog::reportWarning(const std::wstring& msg, bool& warningActive)
{
    PauseTimers dummy(*progressDlg_);

    errorLog_.logMsg(msg, MSG_TYPE_WARNING);

    if (!warningActive)
        return;

    if (!progressDlg_->getOptionIgnoreErrors())
    {
        forceUiRefreshNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

        bool dontWarnAgain = false;
        switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::warning,
                                       PopupDialogCfg().setDetailInstructions(msg).
                                       setCheckBox(dontWarnAgain, _("&Don't show this warning again")),
                                       _("&Ignore")))
        {
            case ConfirmationButton::accept:
                warningActive = !dontWarnAgain;
                break;
            case ConfirmationButton::cancel:
                userAbortProcessNow(); //throw AbortProcess
                break;
        }
    }
    //else: if errors are ignored, then warnings should be, too
}


ProcessCallback::Response StatusHandlerFloatingDialog::reportError(const std::wstring& msg, size_t retryNumber)
{
    PauseTimers dummy(*progressDlg_);

    //auto-retry
    if (retryNumber < automaticRetryCount_)
    {
        errorLog_.logMsg(msg + L"\n-> " + _("Automatic retry"), MSG_TYPE_INFO);
        delayAndCountDown(_("Automatic retry") + (automaticRetryCount_ <= 1 ? L"" :  L" " + numberTo<std::wstring>(retryNumber + 1) + L"/" + numberTo<std::wstring>(automaticRetryCount_)),
        automaticRetryDelay_, [&](const std::wstring& statusMsg) { this->reportStatus(_("Error") + L": " + statusMsg); }); //throw AbortProcess
        return ProcessCallback::retry;
    }

    //always, except for "retry":
    auto guardWriteLog = zen::makeGuard<ScopeGuardRunMode::ON_EXIT>([&] { errorLog_.logMsg(msg, MSG_TYPE_ERROR); });

    if (!progressDlg_->getOptionIgnoreErrors())
    {
        forceUiRefreshNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

        switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::error,
                                       PopupDialogCfg().setDetailInstructions(msg),
                                       _("&Ignore"), _("Ignore &all"), _("&Retry")))
        {
            case ConfirmationButton3::accept: //ignore
                return ProcessCallback::ignoreError;

            case ConfirmationButton3::acceptAll: //ignore all
                progressDlg_->setOptionIgnoreErrors(true);
                return ProcessCallback::ignoreError;

            case ConfirmationButton3::decline: //retry
                guardWriteLog.dismiss();
                errorLog_.logMsg(msg + L"\n-> " + _("Retrying operation..."), MSG_TYPE_INFO); //explain why there are duplicate "doing operation X" info messages in the log!
                return ProcessCallback::retry;

            case ConfirmationButton3::cancel:
                userAbortProcessNow(); //throw AbortProcess
                break;
        }
    }
    else
        return ProcessCallback::ignoreError;

    assert(false);
    return ProcessCallback::ignoreError; //dummy value
}


void StatusHandlerFloatingDialog::reportFatalError(const std::wstring& msg)
{
    PauseTimers dummy(*progressDlg_);

    errorLog_.logMsg(msg, MSG_TYPE_FATAL_ERROR);

    if (!progressDlg_->getOptionIgnoreErrors())
    {
        forceUiRefreshNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

        switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::error,
                                       PopupDialogCfg().setTitle(_("Serious Error")).
                                       setDetailInstructions(msg),
                                       _("&Ignore"), _("Ignore &all")))
        {
            case ConfirmationButton2::accept:
                break;

            case ConfirmationButton2::acceptAll:
                progressDlg_->setOptionIgnoreErrors(true);
                break;

            case ConfirmationButton2::cancel:
                userAbortProcessNow(); //throw AbortProcess
                break;
        }
    }
}


void StatusHandlerFloatingDialog::updateDataProcessed(int itemsDelta, int64_t bytesDelta)
{
    StatusHandler::updateDataProcessed(itemsDelta, bytesDelta);

    //note: this method should NOT throw in order to properly allow undoing setting of statistics!
    progressDlg_->notifyProgressChange(); //noexcept
    //for "curveDataBytes_->addRecord()"
}


void StatusHandlerFloatingDialog::forceUiRefreshNoThrow()
{
    progressDlg_->updateGui();
}
