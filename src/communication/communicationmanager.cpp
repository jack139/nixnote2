/*********************************************************************************
NixNote - An open-source client for the Evernote service.
Copyright (C) 2013 Randy Baumgarte

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
***********************************************************************************/


#include "communicationmanager.h"
#include "src/oauth/oauthtokenizer.h"
#include "src/global.h"

// Windows Check
#ifndef _WIN32

#include <execinfo.h>

#endif  // end windows check

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include "src/sql/resourcetable.h"
#include "src/sql/tagtable.h"
#include "src/sql/usertable.h"
#include "src/sql/notetable.h"
#include <QPainter>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

// Windows Check
#ifndef _WIN32

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <execinfo.h>

#include <curl/curl.h>
#include <curl/easy.h>

#endif // End windows check

#include "src/utilities/debugtool.h"

extern Global global;


// Generic constructor
CommunicationManager::CommunicationManager(DatabaseConnection *db) {
    this->db = db;
    evernoteHost = global.server;
    //userStorePath = "/edam/user";
    //clientName = NN_APP_CLIENT_NAME;
    inkNoteList = new QList<QPair<QString, QImage *> *>();
    thumbnailList = new QList<QPair<QString, QImage *> *>();
    postData = new QUrl();
    tagGuidMap = new QHash<QString, QString>;
    initComplete = false;
    noteStore = nullptr;
    myNoteStore = nullptr;
    linkedNoteStore = nullptr;
    minutesToNextSync = 0;
    if (networkAccessManager == nullptr) {
        networkAccessManager = new QNetworkAccessManager(this);
        //        connect(networkAccessManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(inkNoteFinished(QNetworkReply*)));
    }
}


// Destructor
CommunicationManager::~CommunicationManager() {
    delete postData;
    delete tagGuidMap;
}



//***********************************************************************
//***********************************************************************
//*** Init & default user connection routines                         ***
//***********************************************************************
//***********************************************************************



// Connect to Evernote
bool CommunicationManager::enConnect() {
    // Get the oAuth token
    OAuthTokenizer tokenizer;
    QString data = global.accountsManager->getOAuthToken();
    tokenizer.tokenize(data);
    authToken = tokenizer.oauth_token;
//    authToken = global.accountsManager->getOAuthToken();
    return init();
}


// start the communication session
bool CommunicationManager::init() {
    if (initComplete)
        return true;
    if (!initNoteStore())
        return false;
    initComplete = true;
    return true;
}


// Initialize the note store
bool CommunicationManager::initNoteStore() {
    using namespace qevercloud;
    QLOG_DEBUG() << "Inside CommunicationManager::initNoteStore()";

    User user;
    if (!getUserInfo(user))
        return false;
    noteStorePath = "/edam/note/" + user.shardId;

    QString noteStoreUrl = QString("https://") + evernoteHost + noteStorePath;
    myNoteStore = new NoteStore(noteStoreUrl, authToken, this);
    noteStore = myNoteStore;
    return true;
}


// Disconnect from Evernote's servers (for private notebooks)
void CommunicationManager::enDisconnect() {
    //noteStore->disconnect();
    userStore->disconnect();
    if (linkedNoteStore != nullptr)
        linkedNoteStore->disconnect();
    if (myNoteStore != nullptr)
        myNoteStore->disconnect();
}


// Get a user's information
bool CommunicationManager::getUserInfo(User &user) {
    QLOG_DEBUG() << "Inside CommunicationManager::getUserInfo";
    userStore = new UserStore(evernoteHost, authToken);
    try {
        User u = userStore->getUser();
        user = u;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    } catch (const std::exception &e) {
        reportError(CommunicationError::StdException, 16, e.what());
        return false;
    }

    QLOG_TRACE_OUT();
    return true;
}






//***********************************************************************
//***********************************************************************
//*** Sync state & sync functions - non linked                        ***
//***********************************************************************
//***********************************************************************




// Get the current sync state
bool CommunicationManager::getSyncState(QString token, SyncState &syncState) {
    if (token == "")
        token = authToken;
    try {
        if (token == authToken)
            noteStore = myNoteStore;
        else
            noteStore = linkedNoteStore;
        syncState = noteStore->getSyncState(authToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    }
    return true;
}


// Get a sync chunk
bool
CommunicationManager::getSyncChunk(SyncChunk &chunk, int start, int chunkSize, int type, bool fullSync, QString token) {
    if (token == "")
        token = authToken;
    noteStore = myNoteStore;
    // Get rid of old stuff from last chunk
    while (inkNoteList->size() > 0) {
        QPair<QString, QImage *> *pair = inkNoteList->takeLast();
        delete pair->second;
        delete pair;
    }
    inkNoteList->empty();

    bool notebooks = false;
    bool searches = false;
    bool tags = false;
    bool linkedNotebooks = false;
    bool notes = false;
    bool resources = false;
    bool expunged = false;

    notebooks = ((type & SYNC_CHUNK_NOTEBOOKS) > 0);
    searches = ((type & SYNC_CHUNK_SEARCHES) > 0);
    tags = ((type & SYNC_CHUNK_TAGS) > 0);
    linkedNotebooks = ((type & SYNC_CHUNK_LINKED_NOTEBOOKS) > 0);
    notes = ((type & SYNC_CHUNK_NOTES) > 0);
    expunged = ((type & SYNC_CHUNK_NOTES) && (!fullSync) > 0) | (SYNC_CHUNK_EXPUNGED && (!fullSync));
    resources = ((type & SYNC_CHUNK_RESOURCES) && (!fullSync) > 0);

    // Try to get the chunk
    SyncChunkFilter filter;

    filter.includeExpunged = expunged;
    filter.includeNotes = notes;
    filter.includeNoteResources = fullSync;
    filter.includeNoteAttributes = notes;
    filter.includeNotebooks = notebooks;
    filter.includeTags = tags;
    filter.includeSearches = searches;
    filter.includeResources = resources;
    filter.includeLinkedNotebooks = linkedNotebooks;
    filter.includeNoteApplicationDataFullMap = false;
    filter.includeNoteResourceApplicationDataFullMap = false;
    filter.includeNoteResourceApplicationDataFullMap = false;

    // This is a failsafe to prevnt loops if nothing passes the filter
    chunk.chunkHighUSN = chunk.updateCount;
    try {
        chunk = myNoteStore->getFilteredSyncChunk(start, chunkSize, filter, token);
        processSyncChunk(chunk, token);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
    return true;
}


// Upload a new/changed saved search
qint32 CommunicationManager::uploadSavedSearch(SavedSearch &search) {
    try {
        if (search.updateSequenceNum > 0)
            return myNoteStore->updateSearch(search, authToken);
        else
            search = myNoteStore->createSearch(search, authToken);
        return search.updateSequenceNum;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        DebugTool d;
        d.dumpSavedSearch(search);
        return 0;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        DebugTool d;
        d.dumpSavedSearch(search);
        return 0;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        DebugTool d;
        d.dumpSavedSearch(search);
        return 0;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        DebugTool d;
        d.dumpSavedSearch(search);
        return false;
    }
}


// Permanently delete a saved search
qint32 CommunicationManager::expungeSavedSearch(Guid guid) {
    try {
        return myNoteStore->expungeSearch(guid, authToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return 0;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return 0;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException &e) {
        return 1;
    }
}


// Upload a new/changed tag to Evernote
qint32 CommunicationManager::uploadTag(Tag &tag) {
    QLOG_TRACE_IN();

    QString additionalInfo("Tag name: ");
    additionalInfo.append(tag.name);
    try {
        if (tag.updateSequenceNum > 0) {
            QLOG_TRACE_OUT();
            return myNoteStore->updateTag(tag, authToken);
        } else {
            tag = myNoteStore->createTag(tag, authToken);
            QLOG_TRACE_OUT();
            return tag.updateSequenceNum;
        }
    } catch (ThriftException &e) {
        QString msg(e.what());
        msg.append(" # ").append(additionalInfo);
        reportError(CommunicationError::ThriftException, e.type(), msg);
        DebugTool d;
        d.dumpTag(tag);
        return 0;
    } catch (EDAMUserException &e) {
        QString msg(e.what());
        msg.append(" # ").append(additionalInfo);
        reportError(CommunicationError::EDAMUserException, e.errorCode, msg);
        DebugTool d;
        d.dumpTag(tag);
        return 0;
    } catch (EDAMSystemException &e) {
        DebugTool d;
        d.dumpTag(tag);
        handleEDAMSystemException(e, additionalInfo);
        return 0;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e, additionalInfo);
        DebugTool d;
        d.dumpTag(tag);
        return 0;
    }
}


// Permanently delete a tag from Evernote
qint32 CommunicationManager::expungeTag(Guid guid) {
    try {
        return myNoteStore->expungeTag(guid, authToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return 0;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return 0;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException) {
        return 1;
    }
}


// Upload a notebook to Evernote
qint32 CommunicationManager::uploadNotebook(Notebook &notebook) {
    try {
        if (notebook.updateSequenceNum > 0)
            return myNoteStore->updateNotebook(notebook, authToken);
        else {
            notebook = myNoteStore->createNotebook(notebook, authToken);
            return notebook.updateSequenceNum;
        }
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        DebugTool d;
        d.dumpNotebook(notebook);
        return 0;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        DebugTool d;
        d.dumpNotebook(notebook);
        return 0;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        DebugTool d;
        d.dumpNotebook(notebook);
        return 0;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        DebugTool d;
        d.dumpNotebook(notebook);
        return false;
    }
}


// Permanently delete a notebook from Evernote
qint32 CommunicationManager::expungeNotebook(Guid guid) {
    try {
        return myNoteStore->expungeNotebook(guid, authToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return 0;
    } catch (EDAMNotFoundException) {
        return 1;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return 0;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return 0;
    }
}


// Upload a note to Evernote
qint32 CommunicationManager::uploadNote(Note &note, QString token) {
    if (token == "") {
        token = authToken;
    }
    noteStore = (token == authToken) ? myNoteStore : linkedNoteStore;
    try {
        if (note.updateSequenceNum.isSet() && note.updateSequenceNum > 0) {
            note = noteStore->updateNote(note, token);
        } else {
            note = noteStore->createNote(note, token);
        }
        return note.updateSequenceNum;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        dumpNote(note);
        return 0;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        dumpNote(note);
        return 0;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e, note.title);
        dumpNote(note);
        return 0;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e, note.title);
        dumpNote(note);
        return 0;
    }
}

void CommunicationManager::reportError(
    const CommunicationError::CommunicationErrorType errorType,
    int code,
    const QString &message,
    const QString &internalMessage) {
#ifndef _WIN32
    // non Windows only
    void *array[30];
    size_t size;
    // get void*'s for all entries on the stack
    size = backtrace(array, 30);

    // print out all the frames to stderr
    QLOG_ERROR() << "Exception stacktrace:";
    backtrace_symbols_fd(array, size, 2);
#endif // End windows check
    error.resetTo(errorType, code, message, internalMessage);

    global.setMessage(tr("Error in sync: ") + error.getMessage(), 0);
}

void CommunicationManager::resetError() {
    error.reset();
}


void CommunicationManager::dumpNote(const Note &note) const {
    DebugTool d;
    d.dumpNote(note);
}


// delete a note in Evernote
qint32 CommunicationManager::deleteNote(Guid note, QString token) {
    if (token == "") {
        token = authToken;
    }
    noteStore = (token == authToken) ? myNoteStore : linkedNoteStore;


    try {
        return noteStore->deleteNote(note, token);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return 0;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return 0;
    } catch (EDAMNotFoundException &e) {
        return 1;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return 0;
    }
}






//***********************************************************************
//***********************************************************************
//*** Linked Notebook function                                        ***
//***********************************************************************
//***********************************************************************

// delete a note in Evernote
qint32 CommunicationManager::deleteLinkedNote(Guid note) {
    return deleteNote(note, linkedAuthToken);
}


// Upload a note to Evernote
qint32 CommunicationManager::uploadLinkedNote(Note &note) {
    return uploadNote(note, linkedAuthToken);
}


// Get a shared notebook by authentication token
bool CommunicationManager::getSharedNotebookByAuth(SharedNotebook &sharedNotebook) {
    try {
        sharedNotebook = noteStore->getSharedNotebookByAuth(linkedAuthToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
    return true;
}


// Authenticate to a linked notebook
bool CommunicationManager::authenticateToLinkedNotebookShard(LinkedNotebook &book) {

    if (!book.noteStoreUrl.isSet()) {
        QLOG_ERROR() << tr("Linked notebook notestore URL missing.");
        return false;
    }

    try {
        if (linkedNoteStore != nullptr)
            delete linkedNoteStore;

        // Connect to the proper shard
        linkedNoteStore = new NoteStore(book.noteStoreUrl, authToken);
        linkedAuthToken = "<Public Notebook>";
        noteStore = linkedNoteStore;

        // Now, authenticate to the book.  Books
        // without a sharekey are public, so authentication
        // isn't needed
        if (!book.shareKey.isSet())
            return true;

        // We have a share key, so authenticate
        linkedAuth = noteStore->authenticateToSharedNotebook(book.shareKey, authToken);
        linkedAuthToken = linkedAuth.authenticationToken;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException) {
        // If it is a linked noteboook & it isn't found, then we can just expunge it
        return false;
    }
    return true;
}


// Get a linked notebook's sync state
bool CommunicationManager::getLinkedNotebookSyncState(SyncState &syncState, LinkedNotebook &linkedNotebook) {
    try {
        syncState = linkedNoteStore->getLinkedNotebookSyncState(linkedNotebook, linkedAuthToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
    return true;
}


// Get a linked notebook's sync chunk
bool CommunicationManager::getLinkedNotebookSyncChunk(SyncChunk &chunk, LinkedNotebook &book, int start, int chunkSize,
                                                      bool fullSync) {
    try {
        chunk = linkedNoteStore->getLinkedNotebookSyncChunk(book, start, chunkSize, fullSync, authToken);
        processSyncChunk(chunk, linkedAuthToken);
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
    return true;
}







//***********************************************************************
//***********************************************************************
//*** Get earlier versions of a note                                  ***
//***********************************************************************
//***********************************************************************




// get a list of all prior versions of a note
bool CommunicationManager::listNoteVersions(QList<NoteVersionId> &list, QString guid) {
    try {
        list = noteStore->listNoteVersions(guid, authToken);
        return true;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
}


// Get a prior version of a notebook
bool CommunicationManager::getNoteVersion(Note &note, QString guid, qint32 usn, bool withResourceData,
                                          bool withResourceRecognition, bool withResourceAlternateData) {
    try {
        note = noteStore->getNoteVersion(guid, usn,
                                         withResourceData, withResourceRecognition, withResourceAlternateData,
                                         authToken);
        return true;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
}


// Get a prior version of a notebook
bool CommunicationManager::getNote(Note &note, QString guid, bool withResource, bool withResourceRecognition,
                                   bool withResourceAlternateData) {
    try {
        note = noteStore->getNote(guid, true, withResource, withResourceRecognition, withResourceAlternateData,
                                  authToken);
        return true;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
}






//***********************************************************************
//***********************************************************************
//*** General listing functions.  Get notebooks & tags                ***
//***********************************************************************
//***********************************************************************



// get a list of all notebooks
bool CommunicationManager::getNotebookList(QList<Notebook> &list) {
    QList<Notebook> retval;
    try {
        retval = noteStore->listNotebooks(authToken);
        list = retval;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
    return true;
}


// get a list of all Tags
bool CommunicationManager::getTagList(QList<Tag> &list) {
    QList<Tag> retval;
    try {
        retval = noteStore->listTags(authToken);
        list = retval;
    } catch (ThriftException &e) {
        reportError(CommunicationError::ThriftException, e.type(), e.what());
        return false;
    } catch (EDAMUserException &e) {
        reportError(CommunicationError::EDAMUserException, e.errorCode, e.what());
        return false;
    } catch (EDAMSystemException &e) {
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException &e) {
        handleEDAMNotFoundException(e);
        return false;
    }
    return true;
}




//***********************************************************************
//***********************************************************************
//*** Ink note routines                                               ***
//***********************************************************************
//***********************************************************************


// See if there are any ink notes in this list of resources
void CommunicationManager::checkForInkNotes(QList<Resource> &resources, QString shard, QString authToken) {
    for (int i = 0; i < resources.size(); i++) {
        Resource *r = &resources[i];
        QString mime = "";
        if (r->mime.isSet())
            mime = r->mime;
        if (mime == "application/vnd.evernote.ink") {
            downloadInkNoteImage(r->guid, r, shard, authToken);
        }
    }
}


// Writer function called when curl has an image ready
size_t curlWriter(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written;
    written = fwrite(ptr, size, nmemb, stream);
    return written;
}


// Download an ink note image
void CommunicationManager::downloadInkNoteImage(QString guid, Resource *r, QString shard, QString authToken) {
// Windows Check
#ifdef _WIN32
    Q_UNUSED(guid)
    Q_UNUSED(r)
    Q_UNUSED(shard)
    Q_UNUSED(authToken)
#else
    UserTable userTable(db);
    QImage *newImage = nullptr;
    User u;
    userTable.getUser(u);
    if (shard == "")
        shard = u.shardId;
    QString urlBase = QString("https://") + evernoteHost
                      + QString("/shard/")
                      + shard
                      + QString("/res/")
                      + guid + QString(".ink?slice=");
    int sliceCount = 1 + ((r->height - 1) / 600);

    QSize size;
    size.setHeight(r->height);
    size.setWidth(r->width);

#if QT_VERSION < 0x050000
    QUrl postData;
#else
    QUrlQuery postData;
#endif
    postData.clear();
    postData.addQueryItem("auth", authToken);

    CURL *curl;
    FILE *fp;
    CURLcode res;

    curl = curl_easy_init();
    if (curl) {
        int position = 0;
        for (int i = 0; i < sliceCount && position >= 0; i++) {

            QTemporaryFile tempFile;
            tempFile.open();
            tempFile.close();
            fp = fopen(tempFile.fileName().toStdString().c_str(), "wb");

            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriter);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

#if QT_VERSION < 0x050000
            QString url = urlBase+QString::number(i+1)+"&"+postData.encodedQuery();
#else
            QString url = urlBase + QString::number(i + 1) + "&" + postData.query();
#endif
            curl_easy_setopt(curl, CURLOPT_URL, url.toStdString().c_str());
            res = curl_easy_perform(curl);
            QLOG_DEBUG() << "curl inknote result " << res;
            fclose(fp);

            // Now we have the file, let's read it and add it to the final image
            QImage replyImage;
            replyImage.load(tempFile.fileName(), "PNG");
            tempFile.remove();
            if (newImage == nullptr) {
                newImage = new QImage(size, replyImage.format());
            }
            position = inkNoteReady(newImage, &replyImage, position);
        }

        // Cleanup
        curl_easy_cleanup(curl);


        // Start writing the resource
        QPair<QString, QImage *> *newPair = new QPair<QString, QImage *>();
        newPair->first = guid;
        newPair->second = newImage;
        inkNoteList->append(newPair);
    }
#endif // End windows check
}


// An ink note image is ready for retrieval
int CommunicationManager::inkNoteReady(QImage *img, QImage *replyImage, int position) {
    int priorPosition = position;
    position = position + replyImage->height();
    if (!replyImage->isNull()) {
        QPainter p(img);
        //        p.drawImage(QRect(0,priorPosition, replyImage->width(), position), *replyImage);
        p.drawImage(QRect(0, priorPosition, replyImage->width(), replyImage->height()), *replyImage);
        p.end();
        return position;
    }
    return -1;
}


//***********************************************************************
//***********************************************************************
//* Take a sync chunk & get all the missing stuff
//***********************************************************************
//***********************************************************************
void CommunicationManager::processSyncChunk(SyncChunk &chunk, QString token) {
    QHash<QString, QString> noteList;
    QList<Note> notes;
    if (chunk.notes.isSet())
        notes = chunk.notes;
    for (int i = 0; i < notes.size(); i++) {
        QLOG_TRACE() << "Fetching chunk item: " << i << ": " << notes[i].title;
        Note n = notes[i];
        noteList.insert(n.guid, "");
        n = noteStore->getNote(notes[i].guid, true, true, true, true, token);
        QLOG_TRACE() << "Note Retrieved";

        // Load up the tag names because Evernote doesn't give them.
        QList<QString> tagNames;
        QList<QString> tagGuids;
        if (n.tagGuids.isSet())
            tagGuids = n.tagGuids;
        for (int j = 0; j < tagGuids.size(); j++) {
            QString tagGuid = tagGuids[j];
            if (tagGuidMap->contains(tagGuid)) {
                QString tagName = tagGuidMap->value(tagGuid);
                tagNames.append(tagName);
            }
            n.tagNames = tagNames;
        }
        QList<Resource> resources;
        if (n.resources.isSet())
            resources = n.resources;
        if (resources.size() > 0) {
            QLOG_TRACE() << "Checking for ink note";
            checkForInkNotes(n.resources, "", authToken);
        }
        notes[i] = n;
    }
    if (chunk.notes.isSet())
        chunk.notes = notes;

    QList<Resource> resourceData;
    QLOG_DEBUG() << "All notes retrieved.  Getting resources";
    QList<Resource> resources;
    if (chunk.resources.isSet())
        resources = chunk.resources;
    for (int i = 0; i < resources.size(); i++) {
        QLOG_TRACE() << "Fetching chunk resource item: " << i << ": " << resources[i].guid;
        Resource r;
        r = noteStore->getResource(resources[i].guid, true, true, true, true, token);
        QLOG_TRACE() << "Resource retrieved";
        resourceData.append(r);
    }
    if (chunk.resources.isSet())
        chunk.resources = resourceData;
    QLOG_DEBUG() << "Getting ink notes";
    if (resources.size() > 0) {
        QLOG_TRACE() << "Checking for ink notes";
        checkForInkNotes(resources, "", token);
    }
}




//***********************************************************************
//***********************************************************************
//*** Exception Handling. Print trace information & return            ***
//***********************************************************************
//***********************************************************************

// Error handler EDAM System Exception
void CommunicationManager::handleEDAMSystemException(EDAMSystemException e, QString additionalInfo) {
    QString msg;
    msg.append(e.what());
    if (e.message.isSet()) {
        msg.append(" # ").append(e.message.ref());
    }
    if (!additionalInfo.isEmpty()) {
        msg.append(" # ").append(additionalInfo);
    }

    if (e.errorCode == EDAMErrorCode::RATE_LIMIT_REACHED) {
        this->minutesToNextSync = e.rateLimitDuration / 60 + 1;


        string endOfText = "minute";
        if (this->minutesToNextSync > 1)
            endOfText = "minutes";

        global.settings->beginGroup(INI_GROUP_SYNC);
        bool apiRateLimitAutoRestart = global.settings->value("apiRateLimitAutoRestart", false).toBool();
        global.settings->endGroup();

        string startText = "Enable 'Sync' > 'Restart sync on API limit' or try again in";
        if (apiRateLimitAutoRestart)
            startText = "Application will continue to sync in";

        QString userMessage(
            tr("API rate limit exceeded.") + QString(" ")
            + tr(startText.c_str()) + QString(" ") + QString::number(this->minutesToNextSync)
            + " " + tr(endOfText.c_str()));
        if (e.rateLimitDuration.isSet()) {
            msg.append(" # rateLimitDuration=").append(e.rateLimitDuration.ref());
        }
        reportError(CommunicationError::RateLimitExceeded, e.errorCode, userMessage, msg);
        return;
    }

    reportError(CommunicationError::EDAMSystemException, e.errorCode, msg);
}

// Error handler EDAM Not Found exception.
void CommunicationManager::handleEDAMNotFoundException(EDAMNotFoundException e, QString additionalInfo) {
    QString msg(e.what());
    if (!additionalInfo.isEmpty()) {
        msg.append(" # ");
        msg.append(additionalInfo);
    }
    reportError(CommunicationError::EDAMNotFoundException, 16, msg);
}


void CommunicationManager::loadTagGuidMap() {
    TagTable tagTable(db);
    tagTable.getGuidMap(*this->tagGuidMap);
}


qint32 CommunicationManager::getMinutesToNextSync() {
    return this->minutesToNextSync;
}

int CommunicationManager::getLastErrorCode() const {
    return error.getCode();
}

CommunicationError::CommunicationErrorType CommunicationManager::getLastErrorType() const {
    return error.getType();
}