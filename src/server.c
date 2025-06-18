// CodeSync_server.c
// Windows (Winsock2) 기반 TCP 서버 - 멀티스레드 처리
// 주요 기능: 사용자 인증, 프로젝트 생성, 커밋 저장, 히스토리 조회, 검색, 커밋 무효화, 코드 내용 조회, 동기화 알림
// 컴파일: gcc server.c -o CodeSync_server -lws2_32 (MinGW 환경)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <time.h>
#include <direct.h>
#include <errno.h>

#pragma comment(lib, "ws2_32.lib")

#define BUF_SIZE 4096
#define MAX_CLNT 256
#define MAX_MSG_LEN 10240

// 클라이언트 정보 구조체 (소켓과 사용자 ID 저장)
typedef struct {
    SOCKET sock;
    char id[30];
} ClientInfo;

// 함수 프로토타입 선언
unsigned WINAPI HandleClient(void* arg);
void ErrorHandling(const char* msg);
int AuthenticateUser(const char* id, const char* pw);
void CreateProject(const char* projectName, SOCKET sock);
void ProcessCommit(const char* data, SOCKET sock, const char* userID);
void SendHistory(const char* projectName, SOCKET sock);
void SearchCommits(const char* projectName, const char* query, SOCKET sock);
void RevokeCommit(const char* projectName, const char* commitIdStr, SOCKET sock);
void SendCommitCode(const char* projectName, const char* commitIdStr, SOCKET sock);
void BroadcastUpdate(const char* projectName, SOCKET from_sock);

// 전역 변수 (연결된 클라이언트 목록 및 동기화 제어)
int clntCount = 0;
ClientInfo clntList[MAX_CLNT];
HANDLE hMutex;

int main(int argc, char* argv[]) {
    WSADATA wsaData;
    SOCKET servSock, clntSock;
    SOCKADDR_IN servAddr, clntAddr;
    int clntAddrSize;
    HANDLE hThread;

    if (argc != 2) {
        printf("사용법: %s <포트번호>\n", argv[0]);
        exit(1);
    }

    // Winsock 초기화
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        ErrorHandling("WSAStartup() 오류");
    
    // 서버 소켓 생성
    servSock = socket(PF_INET, SOCK_STREAM, 0);
    if (servSock == INVALID_SOCKET)
        ErrorHandling("socket() 오류");
    
    // 주소 설정
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(atoi(argv[1]));

    // 소켓 바인딩 및 연결 대기
    if (bind(servSock, (SOCKADDR*)&servAddr, sizeof(servAddr)) == SOCKET_ERROR)
        ErrorHandling("bind() 오류");
    if (listen(servSock, 5) == SOCKET_ERROR)
        ErrorHandling("listen() 오류");
    
    // 동기화 객체 생성
    hMutex = CreateMutex(NULL, FALSE, NULL);

    // 사용자 정보 파일 준비 (users.txt 없으면 생성)
    FILE* userFile = fopen("users.txt", "a+");
    if (!userFile)
        ErrorHandling("users.txt 파일 열기 오류");
    
    // 파일 크기가 0이면 기본 사용자 계정 추가
    fseek(userFile, 0, SEEK_END);
    if (ftell(userFile) == 0) {
        fputs("samel123\t1234\n", userFile);
        fputs("guest\t1234\n", userFile);
    }
    fclose(userFile);
    printf("CodeSync 서버 시작...\n");

    // 클라이언트 연결 수락 루프 (다중 클라이언트 처리)
    while (1) {
        clntAddrSize = sizeof(clntAddr);
        clntSock = accept(servSock, (SOCKADDR*)&clntAddr, &clntAddrSize);
        if (clntSock == INVALID_SOCKET) {
            fprintf(stderr, "accept() 오류: %d\n", WSAGetLastError());
            continue;
        }

        // 새로운 클라이언트 접속 시 스레드 생성하여 처리
        hThread = (HANDLE)_beginthreadex(NULL, 0, HandleClient, (void*)&clntSock, 0, NULL);
        if (hThread == 0) {
            fprintf(stderr, "스레드 생성 실패\n");
            closesocket(clntSock);
        } else {
            CloseHandle(hThread);
        }
    }
    closesocket(servSock);
    WSACleanup();
    return 0;
}

// 클라이언트 전용 스레드 함수: 로그인 처리 및 명령 처리
unsigned WINAPI HandleClient(void* arg) {
    SOCKET hClntSock = *((SOCKET*)arg);
    char recvBuf[MAX_MSG_LEN];
    int recvLen;
    char userID[30] = "";


    // 1. 로그인 처리 (로그인 성공할 때까지 반복)
    while (1) {
        recvLen = recv(hClntSock, recvBuf, BUF_SIZE - 1, 0);
        if (recvLen <= 0) {
            // 클라이언트 연결 종료 또는 오류 (로그인 단계)
            closesocket(hClntSock);
            return 0;
        }
        recvBuf[recvLen] = '\0';
        // 수신된 로그인 정보 파싱 (ID와 PW는 개행으로 구분)
        char *idToken = strtok(recvBuf, "\n");
        char *pwToken = strtok(NULL, "\n");
        if (!idToken || !pwToken) {
            // 형식이 잘못된 경우, 재요청
            send(hClntSock, "LOGIN_FAIL##END##\n", 18, 0);
            continue;
        }
        if (AuthenticateUser(idToken, pwToken)) {
            // 인증 성공
            strcpy(userID, idToken);
            send(hClntSock, "LOGIN_SUCCESS##END##\n", 21, 0);
            break;
        } else {
            // 인증 실패 - 클라이언트에 알리고 다시 입력 대기
            send(hClntSock, "LOGIN_FAIL##END##\n", 18, 0);
            // 루프 반복 (사용자에게 재입력 기회 제공)
            continue;
        }
    }

    // 사용자 인증 성공 후
    // 전역 클라이언트 목록에 추가
    WaitForSingleObject(hMutex, INFINITE);
    if (clntCount < MAX_CLNT) {
        clntList[clntCount].sock = hClntSock;
        strncpy(clntList[clntCount].id, userID, sizeof(clntList[clntCount].id)-1);
        clntList[clntCount].id[sizeof(clntList[clntCount].id)-1] = '\0';
        clntCount++;
    }
    ReleaseMutex(hMutex);

    // 접속 로그 출력
    printf("클라이언트 접속: %s (ID: %s)\n", userID, userID);


    // 2. 명령 처리 루프
    char msgBuf[MAX_MSG_LEN];
    while ((recvLen = recv(hClntSock, msgBuf, MAX_MSG_LEN - 1, 0)) > 0) {
        msgBuf[recvLen] = '\0';
        // 여러 명령이 한 번에 수신될 수 있으므로 구분자 "##END##" 기준으로 분리 처리
        char* ptr = msgBuf;
        while (1) {
            char* endMarker = strstr(ptr, "##END##");
            if (endMarker == NULL) {
                break; // 아직 종료 구분자를 받지 못한 경우 다음 recv 대기
            }

            // 한 명령어 블록 추출
            *endMarker = '\0'; // 임시로 문자열 종료
            char commandLine[MAX_MSG_LEN];
            strncpy(commandLine, ptr, sizeof(commandLine)-1);
            commandLine[sizeof(commandLine)-1] = '\0';
            // endMarker 이후로 남은 문자열을 위해 포인터 이동
            ptr = endMarker + strlen("##END##");
            // 명령어 구문 분석: 첫 줄에 명령, 이후 데이터
            char* cmd = strtok(commandLine, "\n");
            if (!cmd) {
                continue;
            }
            char* data = cmd + strlen(cmd) + 1;

            // 명령어에 따라 처리 함수 호출
            if (strcmp(cmd, "create_project") == 0) {
                CreateProject(data, hClntSock);
            } else if (strcmp(cmd, "commit") == 0) {
                ProcessCommit(data, hClntSock, userID);
            } else if (strcmp(cmd, "history") == 0) {
                // 프로젝트 히스토리 전송
                strtok(data, "\n"); // 개행 제거
                SendHistory(data, hClntSock);
            } else if (strcmp(cmd, "search") == 0) {
                // 프로젝트 내 커밋 제목 검색
                char* projectName = strtok(data, "\n");
                char* query = strtok(NULL, "\n");
                if (projectName && query) {
                    SearchCommits(projectName, query, hClntSock);
                } else {
                    // 형식 오류
                    send(hClntSock, "SEARCH_FAIL##END##\n", 19, 0);
                }
            } else if (strcmp(cmd, "revoke") == 0) {
                char* projectName = strtok(data, "\n");
                char* commitIdStr = strtok(NULL, "\n");
                RevokeCommit(projectName, commitIdStr, hClntSock);
            } else if (strcmp(cmd, "view") == 0) {
                char* projectName = strtok(data, "\n");
                char* commitIdStr = strtok(NULL, "\n");
                SendCommitCode(projectName, commitIdStr, hClntSock);
            } else {
                // 알 수 없는 명령어 처리
                send(hClntSock, "INVALID_COMMAND##END##\n", 23, 0);
            }
        }
    }


    // 3. 연결 종료 처리 (recv <= 0인 경우 루프 탈출)
    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCount; ++i) {
        if (clntList[i].sock == hClntSock) {
            // 리스트에서 제거 (배열 압축)
            for (int j = i; j < clntCount - 1; ++j) {
                clntList[j] = clntList[j + 1];
            }
            clntCount--;
            break;
        }
    }
    ReleaseMutex(hMutex);
    printf("클라이언트 연결 종료: %s\n", userID);
    closesocket(hClntSock);
    return 0;
}

// 사용자 인증 함수: users.txt에서 ID와 PW 비교
int AuthenticateUser(const char* id, const char* pw) {
    FILE* fp = fopen("users.txt", "r");
    if (!fp) {  // 파일 열기 실패 시 인증 실패로 간주
        return 0; 
    }
    char line[100];
    char fileId[30], filePw[30];
    int authSuccess = 0;

    // 파일에서 한 줄씩 읽어서 ID와 PW를 탭으로 구분하여 비교
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';

        // ID와 PW 추출 (탭 구분자 기준)
        char *tabPos = strchr(line, '\t');
        if (!tabPos) {
            continue;
        }
        *tabPos = '\0';
        strncpy(fileId, line, sizeof(fileId)-1);
        fileId[sizeof(fileId)-1] = '\0';
        strncpy(filePw, tabPos+1, sizeof(filePw)-1);
        filePw[sizeof(filePw)-1] = '\0';
        if (strcmp(id, fileId) == 0 && strcmp(pw, filePw) == 0) {
            authSuccess = 1;
            break;
        }
    }
    fclose(fp);
    return authSuccess;
}

// 프로젝트 생성 처리 함수
void CreateProject(const char* projectName, SOCKET sock) {
    // 개행 문자 제거 및 유효성 검사
    char projNameBuf[100];
    strncpy(projNameBuf, projectName, sizeof(projNameBuf)-1);
    projNameBuf[sizeof(projNameBuf)-1] = '\0';

    // 줄바꿈 제거
    projNameBuf[strcspn(projNameBuf, "\n")] = '\0';

    // 프로젝트명이 비었는지 확인
    if (strlen(projNameBuf) == 0) {
        send(sock, "PROJECT_CREATE_FAIL##END##\n", 27, 0);
        return;
    }

    // 디렉토리 생성 시도
    if (_mkdir(projNameBuf) == 0) {
        // 디렉토리 생성 성공 (새 프로젝트 생성)
        send(sock, "PROJECT_CREATED##END##\n", 22, 0);
    } else if (errno == EEXIST) {
        // 이미 디렉토리가 존재하면 참가로 간주
        send(sock, "PROJECT_EXISTS##END##\n", 21, 0);
    } else {
        // 기타 오류
        send(sock, "PROJECT_CREATE_FAIL##END##\n", 27, 0);
    }
}

// 커밋 처리 함수: 제목, 메시지, 코드 받아서 history와 snippet 파일에 기록
void ProcessCommit(const char* data, SOCKET sock, const char* userID) {
    // 데이터 포맷 (##END## 이전까지)
    char* projectName = strtok((char*)data, "\n");
    char* title = strtok(NULL, "\n");
    char* message = strtok(NULL, "\n");
    char* codeBody = NULL;
    if (message != NULL) {
        codeBody = message + strlen(message) + 1;
    }
    if (!projectName || !title || !message || !codeBody) {
        // 필요한 정보가 하나라도 없으면 실패
        send(sock, "COMMIT_FAIL##END##\n", 19, 0);
        return;
    }

    // 현재 시각을 커밋 ID로 사용 (Epoch time)
    time_t commitID = time(NULL);
    char historyPath[256];
    char snippetPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    sprintf(snippetPath, "%s/snippets_%s.txt", projectName, userID);

    // 파일에 기록 (동시 접근 보호)
    WaitForSingleObject(hMutex, INFINITE);
    FILE* hfp = fopen(historyPath, "a");
    FILE* sfp = fopen(snippetPath, "a");
    if (!hfp || !sfp) { // 파일 열기 실패 시 롤백 및 오류 처리
        if (hfp) fclose(hfp);
        if (sfp) fclose(sfp);
        ReleaseMutex(hMutex);
        send(sock, "COMMIT_FAIL##END##\n", 19, 0);
        return;
    }

    // history.txt에 커밋 메타데이터 기록
    fprintf(hfp, "CommitID: %ld | Title: %s | Author: %s | Message: %s\n",
            (long)commitID, title, userID, message);
    fclose(hfp);

    // 사용자별 snippets 파일에 코드 본문 저장 (구분자 포함)
    fprintf(sfp, "--- Commit at %ld ---\n", (long)commitID);
    fprintf(sfp, "%s\n", codeBody);
    fclose(sfp);
    ReleaseMutex(hMutex);

    // 커밋 성공 응답 및 다른 클라이언트에 업데이트 알림
    send(sock, "COMMIT_SUCCESS##END##\n", 22, 0);
    BroadcastUpdate(projectName, sock);
}

// 프로젝트의 커밋 히스토리 전송 함수 (최신순 정렬, 무효화된 커밋 제외)
void SendHistory(const char* projectName, SOCKET sock) {
    char historyPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    WaitForSingleObject(hMutex, INFINITE);
    FILE* fp = fopen(historyPath, "r");
    if (!fp) {  // 히스토리 파일 없음 or 프로젝트에 커밋 없음
        char msg[] = "이 프로젝트에 대한 히스토리가 없습니다.##END##\n";
        send(sock, msg, strlen(msg), 0);
        ReleaseMutex(hMutex);
        return;
    }

    // revoked_commits.txt에서 무효화된 커밋 ID 목록 읽기
    char revokedPath[256];
    sprintf(revokedPath, "%s/revoked_commits.txt", projectName);
    FILE* rfp = fopen(revokedPath, "r");
    long revokedIDs[1000];
    int revokedCount = 0;
    if (rfp) {
        char idBuf[64];
        while (fgets(idBuf, sizeof(idBuf), rfp)) {  // 개행 제거 후 숫자 변환
            revokedIDs[revokedCount] = atol(idBuf);
            revokedCount++;
            if (revokedCount >= 1000) break;
        }
        fclose(rfp);
    }

    // 히스토리 파일의 모든 라인을 읽어 저장
    char line[1024];
    typedef struct {
        long commitId;
        char lineText[1024];
    } CommitLine;
    CommitLine *lines = NULL;
    size_t lineCount = 0, capacity = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0) continue;

        // CommitID 추출
        char *idPos = strstr(line, "CommitID: ");
        long cid = 0;
        if (idPos) {
            cid = atol(idPos + 9);
        }

        // 무효화된 커밋인지 확인
        int isRevoked = 0;
        for (int i = 0; i < revokedCount; ++i) {
            if (cid == revokedIDs[i]) {
                isRevoked = 1;
                break;
            }
        }
        if (isRevoked) continue;

        // 동적 배열에 추가
        if (lineCount >= capacity) {
            size_t newCap = (capacity == 0) ? 50 : capacity * 2;
            CommitLine* newArr = realloc(lines, newCap * sizeof(CommitLine));
            if (!newArr) break;
            lines = newArr;
            capacity = newCap;
        }
        lines[lineCount].commitId = cid;
        strncpy(lines[lineCount].lineText, line, sizeof(lines[lineCount].lineText)-1);
        lines[lineCount].lineText[sizeof(lines[lineCount].lineText)-1] = '\0';
        lineCount++;
    }
    fclose(fp);
    ReleaseMutex(hMutex);
    if (lineCount == 0) {   // 표시할 내역 없음
        char msg[] = "이 프로젝트에 대한 히스토리가 없습니다.##END##\n";
        send(sock, msg, strlen(msg), 0);
        if (lines) free(lines);
        return;
    }

    // 커밋 시간을 기준으로 최신순 정렬 (commitId가 시간값)
    for (size_t i = 0; i < lineCount; ++i) {
        for (size_t j = i + 1; j < lineCount; ++j) {
            if (lines[i].commitId < lines[j].commitId) {
                CommitLine temp = lines[i];
                lines[i] = lines[j];
                lines[j] = temp;
            }
        }
    }

    // 정렬된 커밋 내역을 하나의 버퍼로 합쳐서 전송
    char outBuf[MAX_MSG_LEN];
    outBuf[0] = '\0';
    for (size_t i = 0; i < lineCount; ++i) {    // commitId를 날짜/시간 문자열로 변환
        char timeStr[64] = "";
        if (lines[i].commitId != 0) {
            time_t t = (time_t)lines[i].commitId;
            struct tm *lt = localtime(&t);
            if (lt) {
                strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", lt);
            }
        }
        char formattedLine[1200];
        if (strlen(timeStr) > 0) {
            sprintf(formattedLine, "%s | Time: %s\n", lines[i].lineText, timeStr);
        } else {
            sprintf(formattedLine, "%s\n", lines[i].lineText);
        }
        // outBuf에 누적 (버퍼 크기 체크)
        if (strlen(outBuf) + strlen(formattedLine) < MAX_MSG_LEN - 10) {
            strcat(outBuf, formattedLine);
        } else {    // 만약 버퍼 초과 시, 루프 종료
            break;
        }
    }

    // 정리
    if (lines) free(lines);

    // 종료 구분자 추가 및 전송
    strcat(outBuf, "##END##\n");
    send(sock, outBuf, strlen(outBuf), 0);
}

// 커밋 제목 검색 처리 함수 (완전 일치 검색, 최신순 출력)
void SearchCommits(const char* projectName, const char* query, SOCKET sock) {
    char cleanQuery[256];
    strncpy(cleanQuery, query, sizeof(cleanQuery) - 1);
    cleanQuery[sizeof(cleanQuery) - 1] = '\0';
    cleanQuery[strcspn(cleanQuery, "\r\n")] = '\0';

    char historyPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    WaitForSingleObject(hMutex, INFINITE);
    FILE* fp = fopen(historyPath, "r");
    if (!fp) {  // 프로젝트 히스토리 없을 시
        char msg[] = "검색 결과가 없습니다.##END##\n";
        send(sock, msg, strlen(msg), 0);
        ReleaseMutex(hMutex);
        return;
    }

    // revoked_commits.txt 읽기 (검색 결과에서도 무효화 커밋 제외)
    char revokedPath[256];
    sprintf(revokedPath, "%s/revoked_commits.txt", projectName);
    FILE* rfp = fopen(revokedPath, "r");
    long revokedIDs[1000];
    int revokedCount = 0;
    if (rfp) {
        char idBuf[64];
        while (fgets(idBuf, sizeof(idBuf), rfp)) {
            revokedIDs[revokedCount] = atol(idBuf);
            revokedCount++;
            if (revokedCount >= 1000) break;
        }
        fclose(rfp);
    }

    // 검색 결과를 저장할 동적 배열
    char line[1024];
    typedef struct {
        long commitId;
        char lineText[1024];
    } CommitEntry;
    CommitEntry *results = NULL;
    size_t resCount = 0, resCap = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0) continue;

        // 커밋 ID와 제목 파싱
        char *titlePos = strstr(line, " | Title: ");
        char *authPos = strstr(line, " | Author: ");
        if (!titlePos || !authPos) continue;

        // 제목 추출
        titlePos += 9; // " | Title: " 문자열 다음부터 제목 시작
        char titleBuf[256];
        size_t titleLen = authPos - titlePos;
        if (titleLen >= sizeof(titleBuf)) titleLen = sizeof(titleBuf) - 1;
        strncpy(titleBuf, titlePos, titleLen);
        titleBuf[titleLen] = '\0';

        // 검색어와 완전 일치 비교
        if (strcmp(titleBuf, cleanQuery) != 0) {
            continue;
        }

        // 해당 커밋 ID 추출
        char *idPos = strstr(line, "CommitID: ");
        long cid = 0;
        if (idPos) {
            cid = atol(idPos + 9);
        }

        // 무효화 여부 확인
        int isRevoked = 0;
        for (int i = 0; i < revokedCount; ++i) {
            if (cid == revokedIDs[i]) {
                isRevoked = 1;
                break;
            }
        }
        if (isRevoked) continue;

        // 결과 배열에 추가
        if (resCount >= resCap) {
            size_t newCap = (resCap == 0 ? 20 : resCap * 2);
            CommitEntry *newArr = realloc(results, newCap * sizeof(CommitEntry));
            if (!newArr) break;
            results = newArr;
            resCap = newCap;
        }
        results[resCount].commitId = cid;
        strncpy(results[resCount].lineText, line, sizeof(results[resCount].lineText)-1);
        results[resCount].lineText[sizeof(results[resCount].lineText)-1] = '\0';
        resCount++;
    }
    fclose(fp);
    ReleaseMutex(hMutex);
    if (resCount == 0) {    // 검색 결과 없음
        char msg[] = "검색 결과가 없습니다.##END##\n";
        send(sock, msg, strlen(msg), 0);
        if (results) free(results);
        return;
    }

    // 결과를 커밋 시간 기준으로 내림차순 정렬
    for (size_t i = 0; i < resCount; ++i) {
        for (size_t j = i + 1; j < resCount; ++j) {
            if (results[i].commitId < results[j].commitId) {
                CommitEntry tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    // 결과를 출력 버퍼에 모으기
    char outBuf[MAX_MSG_LEN];
    outBuf[0] = '\0';
    for (size_t i = 0; i < resCount; ++i) { // 시간 정보 변환
        char timeStr[64] = "";
        if (results[i].commitId != 0) {
            time_t t = (time_t)results[i].commitId;
            struct tm *lt = localtime(&t);
            if (lt) {
                strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", lt);
            }
        }
        char formattedLine[1200];
        if (strlen(timeStr) > 0) {
            sprintf(formattedLine, "%s | Time: %s\n", results[i].lineText, timeStr);
        } else {
            sprintf(formattedLine, "%s\n", results[i].lineText);
        }
        if (strlen(outBuf) + strlen(formattedLine) < MAX_MSG_LEN - 10) {
            strcat(outBuf, formattedLine);
        } else {
            break;
        }
    }
    if (results) free(results);
    strcat(outBuf, "##END##\n");
    send(sock, outBuf, strlen(outBuf), 0);
}

// 커밋 무효화 처리 함수 - 해당 커밋ID를 무효화 목록에 기록
void RevokeCommit(const char* projectName, const char* commitIdStr, SOCKET sock) {
    if (!projectName || !commitIdStr || strlen(projectName) == 0 || strlen(commitIdStr) == 0) {
        send(sock, "REVOKE_FAIL##END##\n", 19, 0);
        return;
    }

    // 유효한 숫자 형태의 commitID인지 확인
    long commitId = atol(commitIdStr);
    if (commitId == 0 && strcmp(commitIdStr, "0") != 0) {   // 변환 실패
        send(sock, "REVOKE_FAIL##END##\n", 19, 0);
        return;
    }
    char historyPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    WaitForSingleObject(hMutex, INFINITE);
    FILE* fp = fopen(historyPath, "r");
    if (!fp) {  // 히스토리 파일 없으면 실패
        send(sock, "REVOKE_FAIL_NOT_FOUND##END##\n", 29, 0);
        ReleaseMutex(hMutex);
        return;
    }

    // 커밋 존재 여부 확인 (history.txt 탐색)
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "CommitID: ") && atol(strstr(line, "CommitID: ") + 9) == commitId) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    if (!found) {
        send(sock, "REVOKE_FAIL_NOT_FOUND##END##\n", 29, 0);
        ReleaseMutex(hMutex);
        return;
    }

    // 이미 무효화되었는지 확인 (revoked_commits에 존재하는지)
    char revokedPath[256];
    sprintf(revokedPath, "%s/revoked_commits.txt", projectName);
    FILE* rfp_check = fopen(revokedPath, "r");
    if (rfp_check) {
        char idBuf[64];
        while (fgets(idBuf, sizeof(idBuf), rfp_check)) {
            if (atol(idBuf) == commitId) {
                found = 2;
                break;
            }
        }
        fclose(rfp_check);
    }
    if (found == 2) {   // 이미 무효화된 경우
        send(sock, "REVOKE_FAIL_NOT_FOUND##END##\n", 29, 0);
        ReleaseMutex(hMutex);
        return;
    }

    // revoked_commits.txt에 커밋ID 추가 (무효화 플래그 기록)
    FILE* rfp = fopen(revokedPath, "a");
    if (!rfp) {
        send(sock, "REVOKE_FAIL##END##\n", 19, 0);
        ReleaseMutex(hMutex);
        return;
    }
    fprintf(rfp, "%ld\n", commitId);
    fclose(rfp);
    ReleaseMutex(hMutex);

    // 응답 및 동기화 알림
    send(sock, "REVOKE_SUCCESS##END##\n", 23, 0);
    BroadcastUpdate(projectName, sock);
}

// 특정 커밋의 코드 전문 전송 함수
void SendCommitCode(const char* projectName, const char* commitIdStr, SOCKET sock) {
    if (!projectName || !commitIdStr) { // 프로젝트명이나 ID 없으면 처리 불필요
        return;
    }

    // history.txt에서 해당 CommitID가 존재하는지 확인하고 작성자 찾기
    char historyPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    WaitForSingleObject(hMutex, INFINITE);
    FILE* fp = fopen(historyPath, "r");
    if (!fp) {
        ReleaseMutex(hMutex);
        send(sock, "HISTORY_CODE_FAIL##END##\n", 25, 0);
        return;
    }
    char line[1024];
    char authorID[30] = "";
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "CommitID: ") && strstr(line, commitIdStr)) {
            // 해당 커밋 라인 서칭
            found = 1;
            char *authPos = strstr(line, "Author: ");
            if (authPos) {
                authPos += 8;
                // author 문자열 추출
                char *authEnd = strstr(authPos, " | Message:");
                if (authEnd) {
                    size_t len = authEnd - authPos;
                    if (len >= sizeof(authorID)) len = sizeof(authorID) - 1;
                    strncpy(authorID, authPos, len);
                    authorID[len] = '\0';
                }
            }
            break;
        }
    }
    fclose(fp);

    // 무효화된 커밋인지 확인
    if (found) {
        char revokedPath[256];
        sprintf(revokedPath, "%s/revoked_commits.txt", projectName);
        FILE* rfp = fopen(revokedPath, "r");
        if (rfp) {
            char idBuf[64];
            while (fgets(idBuf, sizeof(idBuf), rfp)) {
                if (atol(idBuf) == atol(commitIdStr)) {
                    found = 0;
                    break;
                }
            }
            fclose(rfp);
        }
    }
    if (!found || authorID[0] == '\0') {
        ReleaseMutex(hMutex);
        send(sock, "HISTORY_CODE_FAIL##END##\n", 25, 0);
        return;
    }

    // 스니펫 파일에서 해당 CommitID 코드 블록 찾기
    char snippetPath[256];
    sprintf(snippetPath, "%s/snippets_%s.txt", projectName, authorID);
    FILE* sfp = fopen(snippetPath, "r");
    if (!sfp) {
        ReleaseMutex(hMutex);
        send(sock, "HISTORY_CODE_FAIL##END##\n", 25, 0);
        return;
    }
    char codeBuf[BUF_SIZE] = "";
    int inBlock = 0;
    char commitMarker[64];
    sprintf(commitMarker, "--- Commit at %s ---", commitIdStr);

    // 커밋 시작 마커 찾아서 그 이후 줄들을 읽기
    while (fgets(line, sizeof(line), sfp)) {
        if (!inBlock) {
            if (strstr(line, commitMarker)) {
                inBlock = 1;
            }
        } else {
            if (strncmp(line, "--- Commit at", 13) == 0) {  // 다음 커밋 블록 시작 -> 현재 블록 끝
                break;
            }
            strcat(codeBuf, line);
        }
    }
    fclose(sfp);
    ReleaseMutex(hMutex);
    // 응답 데이터 구성 및 전송
    char response[MAX_MSG_LEN];
    sprintf(response, "HISTORY_CODE_RESPONSE\n%s##END##\n", codeBuf);
    send(sock, response, strlen(response), 0);
}

// 다른 클라이언트에 업데이트 알림 방송
void BroadcastUpdate(const char* projectName, SOCKET from_sock) {
    char msg[100];
    sprintf(msg, "UPDATE\n%s\n##END##\n", projectName);
    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCount; ++i) {
        if (clntList[i].sock != from_sock) {
            send(clntList[i].sock, msg, strlen(msg), 0);
        }
    }
    ReleaseMutex(hMutex);
}

// 오류 발생 시 메시지 출력 후 프로그램 종료
void ErrorHandling(const char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}