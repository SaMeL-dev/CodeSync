// CodeSync_client.c
// Windows 콘솔 기반 TCP 클라이언트 - 명령어 입력을 통한 협업 기능
// 컴파일: gcc client.c -o CodeSync_client -lws2_32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define BUF_SIZE 4096
#define MAX_MSG_LEN 10240

// 커밋 내역 연결 리스트 노드 구조체
typedef struct CommitNode {
    char data[1024];
    struct CommitNode* next;
} CommitNode;

// 함수 프로토타입
unsigned WINAPI RecvMsg(void* arg);
void ErrorHandling(const char* msg);
void DisplayHistory(char* historyData);
void FreeHistory(CommitNode* head);

// 전역 변수
char currentProject[50] = "";  // 현재 선택된 프로젝트 이름
SOCKET hSock;

int main(int argc, char* argv[]) {
    WSADATA wsaData;
    SOCKADDR_IN servAddr;
    HANDLE hRecvThread;
    char id[30], pw[30];
    char servMsg[BUF_SIZE];
    int strLen;

    if (argc != 3) {
        printf("사용법: %s <서버 IP> <포트번호>\n", argv[0]);
        exit(1);
    }
    // Winsock 초기화
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        ErrorHandling("WSAStartup() 오류");
    // 소켓 생성
    hSock = socket(PF_INET, SOCK_STREAM, 0);
    if (hSock == INVALID_SOCKET)
        ErrorHandling("socket() 오류");
    // 서버 주소 설정
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = inet_addr(argv[1]);
    servAddr.sin_port = htons(atoi(argv[2]));
    // 서버에 연결 요청
    if (connect(hSock, (SOCKADDR*)&servAddr, sizeof(servAddr)) == SOCKET_ERROR)
        ErrorHandling("connect() 오류");
    // 로그인 절차 (성공할 때까지 반복 입력)
    while (1) {
        printf("ID: ");
        fgets(id, sizeof(id), stdin);
        id[strcspn(id, "\n")] = '\0'; // 개행 제거
        printf("PW: ");
        fgets(pw, sizeof(pw), stdin);
        pw[strcspn(pw, "\n")] = '\0';
        // 서버로 ID와 PW 전송 (구분자 포함)
        char loginMsg[BUF_SIZE];
        sprintf(loginMsg, "%s\n%s\n##END##\n", id, pw);
        send(hSock, loginMsg, strlen(loginMsg), 0);
        // 로그인 응답 수신
        strLen = recv(hSock, servMsg, BUF_SIZE - 1, 0);
        if (strLen <= 0) {
            // 연결 종료 또는 오류
            ErrorHandling("서버와의 연결이 종료되었습니다.");
        }
        servMsg[strLen] = '\0';
        if (strstr(servMsg, "LOGIN_SUCCESS")) {
            printf("로그인 성공\n");
            break;
        } else if (strstr(servMsg, "LOGIN_FAIL")) {
            printf("로그인 실패 - 다시 시도하십시오.\n");
            // 루프 반복하여 재입력
            continue;
        }
    }
    // 로그인 성공 후 프로젝트 선택/생성
    printf("참여 또는 생성할 프로젝트 이름 입력: ");
    fgets(currentProject, sizeof(currentProject), stdin);
    currentProject[strcspn(currentProject, "\n")] = '\0';
    // 프로젝트 생성/참가 요청 전송
    char projMsg[BUF_SIZE];
    sprintf(projMsg, "create_project\n%s\n##END##\n", currentProject);
    send(hSock, projMsg, strlen(projMsg), 0);
    // 서버 응답 수신을 별도 스레드로 처리 (비동기 수신)
    hRecvThread = (HANDLE)_beginthreadex(NULL, 0, RecvMsg, (void*)&hSock, 0, NULL);
    if (hRecvThread == 0) {
        ErrorHandling("수신 스레드 생성 오류");
    }
    // 사용자 명령 입력 루프
    char msg[MAX_MSG_LEN];
    while (1) {
        Sleep(1000);
        printf("\n(Project: %s) 명령 입력 (commit, history, revoke, search, quit): ", currentProject);
        if (!fgets(msg, BUF_SIZE, stdin)) {
            // 입력 오류 시 종료
            break;
        }
        msg[strcspn(msg, "\n")] = '\0'; // 개행 제거
        if (strlen(msg) == 0) {
            continue; // 빈 명령 무시
        }
        if (strcmp(msg, "quit") == 0) {
            // 종료 명령
            break;
        } else if (strcmp(msg, "commit") == 0) {
            // 커밋 명령 처리: 제목, 메시지, 코드 입력 받아 전송
            char title[100], commitMsg[512];
            char code[8192] = "";
            char temp[1024];
            printf("커밋 제목: ");
            fgets(title, sizeof(title), stdin);
            title[strcspn(title, "\n")] = '\0';
            printf("커밋 메시지: ");
            fgets(commitMsg, sizeof(commitMsg), stdin);
            commitMsg[strcspn(commitMsg, "\n")] = '\0';
            printf("코드 입력 (마지막 줄에 '##CODE_END##' 입력):\n");
            while (fgets(temp, sizeof(temp), stdin)) {
                if (strcmp(temp, "##CODE_END##\n") == 0) break;
                // 입력된 코드 라인을 누적
                strcat(code, temp);
            }
            sprintf(msg, "commit\n%s\n%s\n%s\n%s##END##\n", currentProject, title, commitMsg, code);
        } else if (strcmp(msg, "history") == 0) {
            // 히스토리 조회 명령
            sprintf(msg, "history\n%s\n##END##\n", currentProject);
        } else if (strcmp(msg, "revoke") == 0) {
            // 커밋 무효화 명령: Commit ID 입력 받아 전송
            char commitID[50];
            printf("취소할 커밋 ID: ");
            fgets(commitID, sizeof(commitID), stdin);
            commitID[strcspn(commitID, "\n")] = '\0';
            sprintf(msg, "revoke\n%s\n%s\n##END##\n", currentProject, commitID);
        } else if (strcmp(msg, "search") == 0) {
            // 검색 명령: 커밋 제목 입력 받아 전송
            char searchTitle[256];
            printf("검색할 커밋 제목: ");
            fgets(searchTitle, sizeof(searchTitle), stdin);
            searchTitle[strcspn(searchTitle, "\r\n")] = '\0';
            sprintf(msg, "search\n%s\n%s\n##END##\n", currentProject, searchTitle);
        } else {
            // 숫자로만 이루어진 입력인 경우 커밋 ID로 간주하여 코드 조회
            int allDigits = 1;
            for (size_t i = 0; i < strlen(msg); ++i) {
                if (msg[i] < '0' || msg[i] > '9') {
                    allDigits = 0;
                    break;
                }
            }
            if (allDigits) {
                char commitID[50];
                strcpy(commitID, msg);
                sprintf(msg, "view\n%s\n%s\n##END##\n", currentProject, commitID);
            } else {
                printf("잘못된 명령어입니다.\n");
                continue;
            }
        }
        // 구성된 명령 메시지 서버로 전송
        send(hSock, msg, strlen(msg), 0);
    }
    // 프로그램 종료 처리
    closesocket(hSock);
    WaitForSingleObject(hRecvThread, INFINITE);
    CloseHandle(hRecvThread);
    WSACleanup();
    return 0;
}

// 수신 스레드: 서버 메시지를 계속 수신하여 처리
unsigned WINAPI RecvMsg(void* arg) {
    SOCKET hSock = *((SOCKET*)arg);
    char servMsg[MAX_MSG_LEN];
    int strLen;
    while ((strLen = recv(hSock, servMsg, MAX_MSG_LEN - 1, 0)) > 0) {
        servMsg[strLen] = '\0';
        // 서버 메시지에서 종료 구분자 위치 확인
        char* endPtr = strstr(servMsg, "##END##");
        if (!endPtr) continue;
        *endPtr = '\0'; // 메시지만 추출
        // 메시지 종류별 처리
        if (strncmp(servMsg, "UPDATE", 6) == 0) {
            // 다른 클라이언트의 업데이트 알림
            char* ptr = strtok(servMsg, "\n");
            ptr = strtok(NULL, "\n"); // 프로젝트 이름 추출
            if (ptr && strcmp(ptr, currentProject) == 0) {
                printf("\n[알림] 프로젝트 '%s'에 변경사항이 발생했습니다. 'history' 명령으로 새로고침하세요.\n", currentProject);
            }
        } else if (strstr(servMsg, "PROJECT_CREATED")) {
            printf("[시스템] 프로젝트 '%s' 생성 완료.\n", currentProject);
        } else if (strstr(servMsg, "PROJECT_EXISTS")) {
            printf("[시스템] 기존 프로젝트 '%s'에 참여했습니다.\n", currentProject);
        } else if (strstr(servMsg, "PROJECT_CREATE_FAIL")) {
            printf("[시스템] 프로젝트 생성 실패 - 유효하지 않은 이름이거나 권한 문제일 수 있습니다.\n");
        } else if (strstr(servMsg, "COMMIT_SUCCESS")) {
            printf("[시스템] 커밋 성공.\n");
        } else if (strstr(servMsg, "REVOKE_SUCCESS")) {
            printf("[시스템] 커밋 취소 성공.\n");
        } else if (strstr(servMsg, "REVOKE_FAIL_NOT_FOUND")) {
            printf("[시스템] 취소할 커밋을 찾지 못했습니다.\n");
        } else if (strstr(servMsg, "HISTORY_CODE_RESPONSE")) {
            // 커밋 코드 상세 출력
            char* codePtr = strstr(servMsg, "\n");
            if (codePtr) {
                codePtr += 1;
                printf("\n--- 커밋 코드 전문 ---\n%s\n--- 코드 끝 ---\n", codePtr);
            }
        } else if (strstr(servMsg, "HISTORY_CODE_FAIL")) {
            printf("[시스템] 해당 커밋의 코드를 찾을 수 없습니다.\n");
        } else {
            // 그 외의 데이터는 히스토리 또는 검색 결과로 간주하여 출력
            DisplayHistory(servMsg);
        }
    }
    // 수신 루프 종료 -> 서버 연결 끊김
    printf("\n서버와의 연결이 종료되었습니다.\n");
    return 0;
}

// 수신한 커밋 히스토리(또는 검색 결과) 데이터를 연결 리스트로 구성하여 출력
void DisplayHistory(char* historyData) {
    printf("\n--- %s 프로젝트 커밋 히스토리 ---\n", currentProject);
    if (strlen(historyData) == 0 || strstr(historyData, "히스토리가 없습니다") || strstr(historyData, "검색 결과가 없습니다")) {
        // 히스토리 없음 또는 검색 결과 없음 메시지 출력
        printf("%s\n", strlen(historyData) == 0 ? "이 프로젝트에 대한 히스토리가 없습니다." : historyData);
        printf("--- 히스토리 끝 ---\n");
        return;
    }
    // 연결 리스트로 라인 분리
    CommitNode* head = NULL;
    CommitNode* tail = NULL;
    char* line = strtok(historyData, "\n");
    while (line != NULL) {
        CommitNode* newNode = (CommitNode*)malloc(sizeof(CommitNode));
        if (!newNode) {
            FreeHistory(head);
            return;
        }
        strncpy(newNode->data, line, sizeof(newNode->data) - 1);
        newNode->data[sizeof(newNode->data) - 1] = '\0';
        newNode->next = NULL;
        if (head == NULL) {
            head = tail = newNode;
        } else {
            tail->next = newNode;
            tail = newNode;
        }
        line = strtok(NULL, "\n");
    }
    // 리스트 내 모든 노드 출력
    CommitNode* cur = head;
    while (cur) {
        printf("%s\n", cur->data);
        cur = cur->next;
    }
    printf("--- 히스토리 끝 ---\n");
    FreeHistory(head);
}

// 연결 리스트 노드 메모리 해제
void FreeHistory(CommitNode* head) {
    CommitNode* temp;
    while (head != NULL) {
        temp = head;
        head = head->next;
        free(temp);
    }
}

// 치명적 오류 발생 시 메시지를 출력하고 종료
void ErrorHandling(const char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}