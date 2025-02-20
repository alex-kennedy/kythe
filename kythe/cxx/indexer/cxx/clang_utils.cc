/*
 * Copyright 2016 The Kythe Authors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kythe/cxx/indexer/cxx/clang_utils.h"

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Lex/Lexer.h"
#include "glog/logging.h"

namespace kythe {
bool isObjCSelector(const clang::DeclarationName& DN) {
  switch (DN.getNameKind()) {
    case clang::DeclarationName::NameKind::ObjCZeroArgSelector:
    case clang::DeclarationName::NameKind::ObjCOneArgSelector:
    case clang::DeclarationName::NameKind::ObjCMultiArgSelector:
      return true;
    default:
      return false;
  }
}

clang::SourceLocation GetLocForEndOfToken(
    const clang::SourceManager& source_manager,
    const clang::LangOptions& lang_options,
    clang::SourceLocation start_location) {
  if (start_location.isMacroID()) {
    start_location = source_manager.getExpansionLoc(start_location);
  }
  return clang::Lexer::getLocForEndOfToken(start_location,
                                           0 /* offset from end of token */,
                                           source_manager, lang_options);
}

namespace {
class DereferenceMemberTemplatesDeclVisitor
    : public clang::ConstDeclVisitor<DereferenceMemberTemplatesDeclVisitor,
                                     void> {
 public:
  DereferenceMemberTemplatesDeclVisitor() = default;

  const clang::Decl* Visit(const clang::Decl* decl) {
    decl_ = decl;
    ConstDeclVisitor::Visit(decl);
    return decl_;
  }

  void VisitFunctionTemplateDecl(const clang::FunctionTemplateDecl* ft) {
    while ((ft = ft->getInstantiatedFromMemberTemplate())) {
      if (ft->getTemplatedDecl() != nullptr) decl_ = ft->getTemplatedDecl();
    }
  }

  void VisitClassTemplateDecl(const clang::ClassTemplateDecl* ct) {
    while ((ct = ct->getInstantiatedFromMemberTemplate())) {
      if (ct->getTemplatedDecl() != nullptr) decl_ = ct->getTemplatedDecl();
    }
  }

  void VisitClassTemplatePartialSpecializationDecl(
      const clang::ClassTemplatePartialSpecializationDecl* ctp) {
    while ((ctp = ctp->getInstantiatedFromMemberTemplate())) {
      decl_ = ctp;
    }
  }

  void VisitVarTemplatePartialSpecializationDecl(
      const clang::VarTemplatePartialSpecializationDecl* vtp) {
    while ((vtp = vtp->getInstantiatedFromMember())) {
      decl_ = vtp;
    }
  }

  void VisitVarTemplateDecl(const clang::VarTemplateDecl* vt) {
    while ((vt = vt->getInstantiatedFromMemberTemplate())) {
      if (vt->getTemplatedDecl() != nullptr) decl_ = vt->getTemplatedDecl();
    }
  }

  void VisitTypeAliasTemplateDecl(const clang::TypeAliasTemplateDecl* at) {
    while ((at = at->getInstantiatedFromMemberTemplate())) {
      if (at->getTemplatedDecl() != nullptr) decl_ = at->getTemplatedDecl();
    }
  }

 private:
  const clang::Decl* decl_;
};

}  // anonymous namespace

const clang::Decl* DereferenceMemberTemplates(const clang::Decl* decl,
                                              bool use_mts) {
  return use_mts ? DereferenceMemberTemplatesDeclVisitor().Visit(decl) : decl;
}

const clang::Decl* FindSpecializedTemplate(const clang::Decl* decl,
                                           bool use_mts) {
  if (const auto* FD = llvm::dyn_cast<const clang::FunctionDecl>(decl)) {
    if (auto* ftsi = FD->getTemplateSpecializationInfo()) {
      if (!ftsi->isExplicitInstantiationOrSpecialization()) {
        return DereferenceMemberTemplates(ftsi->getTemplate(), use_mts);
      }
    }
  } else if (const auto* ctsd =
                 llvm::dyn_cast<const clang::ClassTemplateSpecializationDecl>(
                     decl)) {
    if (!ctsd->isExplicitInstantiationOrSpecialization()) {
      auto primary_or_partial = ctsd->getSpecializedTemplateOrPartial();
      if (const auto* partial =
              primary_or_partial
                  .dyn_cast<clang::ClassTemplatePartialSpecializationDecl*>()) {
        return DereferenceMemberTemplates(partial, use_mts);
      } else if (const auto* primary =
                     primary_or_partial.dyn_cast<clang::ClassTemplateDecl*>()) {
        return DereferenceMemberTemplates(primary, use_mts);
      }
    }
  } else if (const auto* vtsd =
                 llvm::dyn_cast<const clang::VarTemplateSpecializationDecl>(
                     decl)) {
    if (!vtsd->isExplicitInstantiationOrSpecialization()) {
      auto primary_or_partial = vtsd->getSpecializedTemplateOrPartial();
      if (const auto* partial =
              primary_or_partial
                  .dyn_cast<clang::VarTemplatePartialSpecializationDecl*>()) {
        return DereferenceMemberTemplates(partial, use_mts);
      } else if (const auto* primary =
                     primary_or_partial.dyn_cast<clang::VarTemplateDecl*>()) {
        return DereferenceMemberTemplates(primary, use_mts);
      }
    }
  }
  return DereferenceMemberTemplates(decl, use_mts);
}

bool ShouldHaveBlameContext(const clang::Decl* decl) {
  // TODO(zarko): introduce more blameable decls.
  switch (decl->getKind()) {
    case clang::Decl::Kind::Field:
    case clang::Decl::Kind::Var:
      return true;
    default:
      return false;
  }
}

const clang::Stmt* FindLValueHead(const clang::Stmt* stmt) {
  if (stmt == nullptr) return nullptr;
  switch (stmt->getStmtClass()) {
    case clang::Stmt::StmtClass::DeclRefExprClass:
    case clang::Stmt::StmtClass::ObjCIvarRefExprClass:
    case clang::Stmt::StmtClass::MemberExprClass:
      return stmt;
    default:
      return nullptr;
  }
}

const clang::Decl* GetInfluencedDeclFromLValueHead(const clang::Stmt* head) {
  if (head == nullptr) return nullptr;
  if (auto* expr = llvm::dyn_cast_or_null<clang::DeclRefExpr>(head);
      expr != nullptr && expr->getFoundDecl() != nullptr &&
      (expr->getFoundDecl()->getKind() == clang::Decl::Kind::Var ||
       expr->getFoundDecl()->getKind() == clang::Decl::Kind::ParmVar)) {
    return expr->getFoundDecl();
  }
  if (auto* expr = llvm::dyn_cast_or_null<clang::MemberExpr>(head);
      expr != nullptr) {
    if (auto* member = expr->getMemberDecl(); member != nullptr) {
      return member;
    }
  }
  return nullptr;
}
}  // namespace kythe
