#pragma once
#include "../analyzer.hpp"
#include "../config.hpp"
#include "LibLsp/lsp/lsCodeAction.h"
#include "LibLsp/lsp/CodeActionParams.h"
#include "LibLsp/JsonRpc/RequestInMessage.h"
#include "LibLsp/JsonRpc/lsResponseMessage.h"
#include <vector>

namespace td_codeActionCode {
    struct response : public ResponseMessage<std::vector<CodeAction>, response> {};
    struct request : public lsRequest<lsCodeActionParams, request> {
        static constexpr MethodType kMethodInfo = "textDocument/codeAction";
        request() : lsRequest(kMethodInfo) {}
        using Response = response;
    };
};
MAKE_REFLECT_STRUCT(td_codeActionCode::request, jsonrpc, id, method, params);
MAKE_REFLECT_STRUCT(td_codeActionCode::response, jsonrpc, id, result);

/// @p published_lint: the lint diagnostics last published for this buffer's
/// current state, or nullptr to lint it here when a request needs them.
std::vector<CodeAction> provide_code_actions(
    const Analyzer& analyzer, const Config& config,
    const lsCodeActionParams& params,
    const std::vector<ParseDiagInfo>* published_lint = nullptr);
