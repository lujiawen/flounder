//===--- SemanticHighlighting.cpp - ------------------------- ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SemanticHighlighting.h"
#include "Logger.h"
#include "ParsedAST.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceLocation.h"
#include <algorithm>

namespace clang {
namespace clangd {
namespace {

/// Some names are not written in the source code and cannot be highlighted,
/// e.g. anonymous classes. This function detects those cases.
bool canHighlightName(DeclarationName Name) {
  if (Name.getNameKind() == DeclarationName::CXXConstructorName ||
      Name.getNameKind() == DeclarationName::CXXUsingDirective)
    return true;
  auto *II = Name.getAsIdentifierInfo();
  return II && !II->getName().empty();
}

llvm::Optional<HighlightingKind> kindForType(const Type *TP);
llvm::Optional<HighlightingKind> kindForDecl(const NamedDecl *D) {
  if (auto *USD = dyn_cast<UsingShadowDecl>(D)) {
    if (auto *Target = USD->getTargetDecl())
      D = Target;
  }
  if (auto *TD = dyn_cast<TemplateDecl>(D)) {
    if (auto *Templated = TD->getTemplatedDecl())
      D = Templated;
  }
  if (auto *TD = dyn_cast<TypedefNameDecl>(D)) {
    // We try to highlight typedefs as their underlying type.
    if (auto K = kindForType(TD->getUnderlyingType().getTypePtrOrNull()))
      return K;
    // And fallback to a generic kind if this fails.
    return HighlightingKind::Typedef;
  }
  // We highlight class decls, constructor decls and destructor decls as
  // `Class` type. The destructor decls are handled in `VisitTagTypeLoc` (we
  // will visit a TypeLoc where the underlying Type is a CXXRecordDecl).
  if (auto *RD = llvm::dyn_cast<RecordDecl>(D)) {
    // We don't want to highlight lambdas like classes.
    if (RD->isLambda())
      return llvm::None;
    return HighlightingKind::Class;
  }
  if (isa<ClassTemplateDecl>(D) || isa<RecordDecl>(D) ||
      isa<CXXConstructorDecl>(D))
    return HighlightingKind::Class;
  if (auto *MD = dyn_cast<CXXMethodDecl>(D))
    return MD->isStatic() ? HighlightingKind::StaticMethod
                          : HighlightingKind::Method;
  if (isa<FieldDecl>(D))
    return HighlightingKind::Field;
  if (isa<EnumDecl>(D))
    return HighlightingKind::Enum;
  if (isa<EnumConstantDecl>(D))
    return HighlightingKind::EnumConstant;
  if (isa<ParmVarDecl>(D))
    return HighlightingKind::Parameter;
  if (auto *VD = dyn_cast<VarDecl>(D))
    return VD->isStaticDataMember()
               ? HighlightingKind::StaticField
               : VD->isLocalVarDecl() ? HighlightingKind::LocalVariable
                                      : HighlightingKind::Variable;
  if (isa<BindingDecl>(D))
    return HighlightingKind::Variable;
  if (isa<FunctionDecl>(D))
    return HighlightingKind::Function;
  if (isa<NamespaceDecl>(D) || isa<NamespaceAliasDecl>(D) ||
      isa<UsingDirectiveDecl>(D))
    return HighlightingKind::Namespace;
  if (isa<TemplateTemplateParmDecl>(D) || isa<TemplateTypeParmDecl>(D) ||
      isa<NonTypeTemplateParmDecl>(D))
    return HighlightingKind::TemplateParameter;
  return llvm::None;
}
llvm::Optional<HighlightingKind> kindForType(const Type *TP) {
  if (!TP)
    return llvm::None;
  if (TP->isBuiltinType()) // Builtins are special, they do not have decls.
    return HighlightingKind::Primitive;
  if (auto *TD = dyn_cast<TemplateTypeParmType>(TP))
    return kindForDecl(TD->getDecl());
  if (auto *TD = TP->getAsTagDecl())
    return kindForDecl(TD);
  return llvm::None;
}
// Given a set of candidate declarations, if the declarations all have the same
// highlighting kind, return that highlighting kind, otherwise return None.
template <typename IteratorRange>
llvm::Optional<HighlightingKind> kindForCandidateDecls(IteratorRange Decls) {
  llvm::Optional<HighlightingKind> Result;
  for (NamedDecl *Decl : Decls) {
    auto Kind = kindForDecl(Decl);
    if (!Kind || (Result && Kind != Result))
      return llvm::None;
    Result = Kind;
  }
  return Result;
}

// Collects all semantic tokens in an ASTContext.
class HighlightingTokenCollector
    : public RecursiveASTVisitor<HighlightingTokenCollector> {
  std::vector<HighlightingToken> Tokens;
  ParsedAST &AST;

public:
  HighlightingTokenCollector(ParsedAST &AST) : AST(AST) {}

  std::vector<HighlightingToken> collectTokens() {
    Tokens.clear();
    TraverseAST(AST.getASTContext());
    // Add highlightings for macro expansions as they are not traversed by the
    // visitor.
    for (const auto &M : AST.getMacros().Ranges)
      Tokens.push_back({HighlightingKind::Macro, M});
    // Initializer lists can give duplicates of tokens, therefore all tokens
    // must be deduplicated.
    llvm::sort(Tokens);
    auto Last = std::unique(Tokens.begin(), Tokens.end());
    Tokens.erase(Last, Tokens.end());
    // Macros can give tokens that have the same source range but conflicting
    // kinds. In this case all tokens sharing this source range should be
    // removed.
    std::vector<HighlightingToken> NonConflicting;
    NonConflicting.reserve(Tokens.size());
    for (ArrayRef<HighlightingToken> TokRef = Tokens; !TokRef.empty();) {
      ArrayRef<HighlightingToken> Conflicting =
          TokRef.take_while([&](const HighlightingToken &T) {
            // TokRef is guaranteed at least one element here because otherwise
            // this predicate would never fire.
            return T.R == TokRef.front().R;
          });
      // If there is exactly one token with this range it's non conflicting and
      // should be in the highlightings.
      if (Conflicting.size() == 1)
        NonConflicting.push_back(TokRef.front());
      // TokRef[Conflicting.size()] is the next token with a different range (or
      // the end of the Tokens).
      TokRef = TokRef.drop_front(Conflicting.size());
    }
    return NonConflicting;
  }

  bool VisitNamespaceAliasDecl(NamespaceAliasDecl *NAD) {
    // The target namespace of an alias can not be found in any other way.
    addToken(NAD->getTargetNameLoc(), NAD->getAliasedNamespace());
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    if (canHighlightName(ME->getMemberNameInfo().getName()))
      addToken(ME->getMemberLoc(), ME->getMemberDecl());
    return true;
  }

  bool VisitOverloadExpr(OverloadExpr *E) {
    if (canHighlightName(E->getName()))
      addToken(E->getNameLoc(),
               kindForCandidateDecls(E->decls())
                   .getValueOr(HighlightingKind::DependentName));
    return true;
  }

  bool VisitDependentScopeDeclRefExpr(DependentScopeDeclRefExpr *E) {
    if (canHighlightName(E->getDeclName()))
      addToken(E->getLocation(), HighlightingKind::DependentName);
    return true;
  }

  bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr *E) {
    if (canHighlightName(E->getMember()))
      addToken(E->getMemberLoc(), HighlightingKind::DependentName);
    return true;
  }

  bool VisitNamedDecl(NamedDecl *ND) {
    if (canHighlightName(ND->getDeclName()))
      addToken(ND->getLocation(), ND);
    return true;
  }

  bool VisitUsingDecl(UsingDecl *UD) {
    if (auto K = kindForCandidateDecls(UD->shadows()))
      addToken(UD->getLocation(), *K);
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Ref) {
    if (canHighlightName(Ref->getNameInfo().getName()))
      addToken(Ref->getLocation(), Ref->getDecl());
    return true;
  }

  bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
    addToken(TL.getBeginLoc(), TL.getTypedefNameDecl());
    return true;
  }

  bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc TL) {
    if (const TemplateDecl *TD =
            TL.getTypePtr()->getTemplateName().getAsTemplateDecl())
      addToken(TL.getBeginLoc(), TD);
    return true;
  }

  bool VisitTagTypeLoc(TagTypeLoc L) {
    if (L.isDefinition())
      return true; // Definition will be highligthed by VisitNamedDecl.
    if (auto K = kindForType(L.getTypePtr()))
      addToken(L.getBeginLoc(), *K);
    return true;
  }

  bool VisitDecltypeTypeLoc(DecltypeTypeLoc L) {
    if (auto K = kindForType(L.getTypePtr()))
      addToken(L.getBeginLoc(), *K);
    return true;
  }

  bool VisitDependentNameTypeLoc(DependentNameTypeLoc L) {
    addToken(L.getNameLoc(), HighlightingKind::DependentType);
    return true;
  }

  bool VisitTemplateTypeParmTypeLoc(TemplateTypeParmTypeLoc TL) {
    addToken(TL.getBeginLoc(), HighlightingKind::TemplateParameter);
    return true;
  }

  bool TraverseNestedNameSpecifierLoc(NestedNameSpecifierLoc NNSLoc) {
    if (auto *NNS = NNSLoc.getNestedNameSpecifier()) {
      if (NNS->getKind() == NestedNameSpecifier::Namespace ||
          NNS->getKind() == NestedNameSpecifier::NamespaceAlias)
        addToken(NNSLoc.getLocalBeginLoc(), HighlightingKind::Namespace);
    }
    return RecursiveASTVisitor<
        HighlightingTokenCollector>::TraverseNestedNameSpecifierLoc(NNSLoc);
  }

  bool TraverseConstructorInitializer(CXXCtorInitializer *CI) {
    if (const FieldDecl *FD = CI->getMember())
      addToken(CI->getSourceLocation(), FD);
    return RecursiveASTVisitor<
        HighlightingTokenCollector>::TraverseConstructorInitializer(CI);
  }

  bool VisitDeclaratorDecl(DeclaratorDecl *D) {
    // Highlight 'auto' with its underlying type.
    auto *AT = D->getType()->getContainedAutoType();
    if (!AT)
      return true;
    auto K = kindForType(AT->getDeducedType().getTypePtrOrNull());
    if (!K)
      return true;
    addToken(D->getTypeSpecStartLoc(), *K);
    return true;
  }

private:
  void addToken(SourceLocation Loc, HighlightingKind Kind) {
    if (Loc.isInvalid())
      return;
    const auto &SM = AST.getSourceManager();
    if (Loc.isMacroID()) {
      // Only intereseted in highlighting arguments in macros (DEF_X(arg)).
      if (!SM.isMacroArgExpansion(Loc))
        return;
      Loc = SM.getSpellingLoc(Loc);
    }

    // Non top level decls that are included from a header are not filtered by
    // topLevelDecls. (example: method declarations being included from
    // another file for a class from another file).
    // There are also cases with macros where the spelling loc will not be in
    // the main file and the highlighting would be incorrect.
    if (!isInsideMainFile(Loc, SM))
      return;

    auto R = getTokenRange(SM, AST.getASTContext().getLangOpts(), Loc);
    if (!R) {
      // R should always have a value, if it doesn't something is very wrong.
      elog("Tried to add semantic token with an invalid range");
      return;
    }

    Tokens.push_back({Kind, R.getValue()});
  }

  void addToken(SourceLocation Loc, const NamedDecl *D) {
    if (auto K = kindForDecl(D))
      addToken(Loc, *K);
  }
};

// Encode binary data into base64.
// This was copied from compiler-rt/lib/fuzzer/FuzzerUtil.cpp.
// FIXME: Factor this out into llvm/Support?
std::string encodeBase64(const llvm::SmallVectorImpl<char> &Bytes) {
  static const char Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "abcdefghijklmnopqrstuvwxyz"
                              "0123456789+/";
  std::string Res;
  size_t I;
  for (I = 0; I + 2 < Bytes.size(); I += 3) {
    uint32_t X = (Bytes[I] << 16) + (Bytes[I + 1] << 8) + Bytes[I + 2];
    Res += Table[(X >> 18) & 63];
    Res += Table[(X >> 12) & 63];
    Res += Table[(X >> 6) & 63];
    Res += Table[X & 63];
  }
  if (I + 1 == Bytes.size()) {
    uint32_t X = (Bytes[I] << 16);
    Res += Table[(X >> 18) & 63];
    Res += Table[(X >> 12) & 63];
    Res += "==";
  } else if (I + 2 == Bytes.size()) {
    uint32_t X = (Bytes[I] << 16) + (Bytes[I + 1] << 8);
    Res += Table[(X >> 18) & 63];
    Res += Table[(X >> 12) & 63];
    Res += Table[(X >> 6) & 63];
    Res += "=";
  }
  return Res;
}

void write32be(uint32_t I, llvm::raw_ostream &OS) {
  std::array<char, 4> Buf;
  llvm::support::endian::write32be(Buf.data(), I);
  OS.write(Buf.data(), Buf.size());
}

void write16be(uint16_t I, llvm::raw_ostream &OS) {
  std::array<char, 2> Buf;
  llvm::support::endian::write16be(Buf.data(), I);
  OS.write(Buf.data(), Buf.size());
}

// Get the highlightings on \c Line where the first entry of line is at \c
// StartLineIt. If it is not at \c StartLineIt an empty vector is returned.
ArrayRef<HighlightingToken>
takeLine(ArrayRef<HighlightingToken> AllTokens,
         ArrayRef<HighlightingToken>::iterator StartLineIt, int Line) {
  return ArrayRef<HighlightingToken>(StartLineIt, AllTokens.end())
      .take_while([Line](const HighlightingToken &Token) {
        return Token.R.start.line == Line;
      });
}
} // namespace

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, HighlightingKind K) {
  switch (K) {
  case HighlightingKind::Variable:
    return OS << "Variable";
  case HighlightingKind::LocalVariable:
    return OS << "LocalVariable";
  case HighlightingKind::Parameter:
    return OS << "Parameter";
  case HighlightingKind::Function:
    return OS << "Function";
  case HighlightingKind::Method:
    return OS << "Method";
  case HighlightingKind::StaticMethod:
    return OS << "StaticMethod";
  case HighlightingKind::Field:
    return OS << "Field";
  case HighlightingKind::StaticField:
    return OS << "StaticField";
  case HighlightingKind::Class:
    return OS << "Class";
  case HighlightingKind::Enum:
    return OS << "Enum";
  case HighlightingKind::EnumConstant:
    return OS << "EnumConstant";
  case HighlightingKind::Typedef:
    return OS << "Typedef";
  case HighlightingKind::DependentType:
    return OS << "DependentType";
  case HighlightingKind::DependentName:
    return OS << "DependentName";
  case HighlightingKind::Namespace:
    return OS << "Namespace";
  case HighlightingKind::TemplateParameter:
    return OS << "TemplateParameter";
  case HighlightingKind::Primitive:
    return OS << "Primitive";
  case HighlightingKind::Macro:
    return OS << "Macro";
  }
  llvm_unreachable("invalid HighlightingKind");
}

std::vector<LineHighlightings>
diffHighlightings(ArrayRef<HighlightingToken> New,
                  ArrayRef<HighlightingToken> Old) {
  assert(std::is_sorted(New.begin(), New.end()) &&
         "New must be a sorted vector");
  assert(std::is_sorted(Old.begin(), Old.end()) &&
         "Old must be a sorted vector");

  // FIXME: There's an edge case when tokens span multiple lines. If the first
  // token on the line started on a line above the current one and the rest of
  // the line is the equal to the previous one than we will remove all
  // highlights but the ones for the token spanning multiple lines. This means
  // that when we get into the LSP layer the only highlights that will be
  // visible are the ones for the token spanning multiple lines.
  // Example:
  // EndOfMultilineToken  Token Token Token
  // If "Token Token Token" don't differ from previously the line is
  // incorrectly removed. Suggestion to fix is to separate any multiline tokens
  // into one token for every line it covers. This requires reading from the
  // file buffer to figure out the length of each line though.
  std::vector<LineHighlightings> DiffedLines;
  // ArrayRefs to the current line in the highlightings.
  ArrayRef<HighlightingToken> NewLine(New.begin(),
                                      /*length*/ static_cast<size_t>(0));
  ArrayRef<HighlightingToken> OldLine(Old.begin(),
                                      /*length*/ static_cast<size_t>(0));
  auto NewEnd = New.end();
  auto OldEnd = Old.end();
  auto NextLineNumber = [&]() {
    int NextNew = NewLine.end() != NewEnd ? NewLine.end()->R.start.line
                                          : std::numeric_limits<int>::max();
    int NextOld = OldLine.end() != OldEnd ? OldLine.end()->R.start.line
                                          : std::numeric_limits<int>::max();
    return std::min(NextNew, NextOld);
  };

  for (int LineNumber = 0; NewLine.end() < NewEnd || OldLine.end() < OldEnd;
       LineNumber = NextLineNumber()) {
    NewLine = takeLine(New, NewLine.end(), LineNumber);
    OldLine = takeLine(Old, OldLine.end(), LineNumber);
    if (NewLine != OldLine)
      DiffedLines.push_back({LineNumber, NewLine});
  }

  return DiffedLines;
}

bool operator==(const HighlightingToken &L, const HighlightingToken &R) {
  return std::tie(L.R, L.Kind) == std::tie(R.R, R.Kind);
}
bool operator<(const HighlightingToken &L, const HighlightingToken &R) {
  return std::tie(L.R, L.Kind) < std::tie(R.R, R.Kind);
}
bool operator==(const LineHighlightings &L, const LineHighlightings &R) {
  return std::tie(L.Line, L.Tokens) == std::tie(R.Line, R.Tokens);
}

std::vector<HighlightingToken> getSemanticHighlightings(ParsedAST &AST) {
  return HighlightingTokenCollector(AST).collectTokens();
}

std::vector<SemanticHighlightingInformation>
toSemanticHighlightingInformation(llvm::ArrayRef<LineHighlightings> Tokens) {
  if (Tokens.size() == 0)
    return {};

  // FIXME: Tokens might be multiple lines long (block comments) in this case
  // this needs to add multiple lines for those tokens.
  std::vector<SemanticHighlightingInformation> Lines;
  Lines.reserve(Tokens.size());
  for (const auto &Line : Tokens) {
    llvm::SmallVector<char, 128> LineByteTokens;
    llvm::raw_svector_ostream OS(LineByteTokens);
    for (const auto &Token : Line.Tokens) {
      // Writes the token to LineByteTokens in the byte format specified by the
      // LSP proposal. Described below.
      // |<---- 4 bytes ---->|<-- 2 bytes -->|<--- 2 bytes -->|
      // |    character      |  length       |    index       |

      write32be(Token.R.start.character, OS);
      write16be(Token.R.end.character - Token.R.start.character, OS);
      write16be(static_cast<int>(Token.Kind), OS);
    }

    Lines.push_back({Line.Line, encodeBase64(LineByteTokens)});
  }

  return Lines;
}

llvm::StringRef toTextMateScope(HighlightingKind Kind) {
  // FIXME: Add scopes for C and Objective C.
  switch (Kind) {
  case HighlightingKind::Function:
    return "entity.name.function.cpp";
  case HighlightingKind::Method:
    return "entity.name.function.method.cpp";
  case HighlightingKind::StaticMethod:
    return "entity.name.function.method.static.cpp";
  case HighlightingKind::Variable:
    return "variable.other.cpp";
  case HighlightingKind::LocalVariable:
    return "variable.other.local.cpp";
  case HighlightingKind::Parameter:
    return "variable.parameter.cpp";
  case HighlightingKind::Field:
    return "variable.other.field.cpp";
  case HighlightingKind::StaticField:
    return "variable.other.field.static.cpp";
  case HighlightingKind::Class:
    return "entity.name.type.class.cpp";
  case HighlightingKind::Enum:
    return "entity.name.type.enum.cpp";
  case HighlightingKind::EnumConstant:
    return "variable.other.enummember.cpp";
  case HighlightingKind::Typedef:
    return "entity.name.type.typedef.cpp";
  case HighlightingKind::DependentType:
    return "entity.name.type.dependent.cpp";
  case HighlightingKind::DependentName:
    return "entity.name.other.dependent.cpp";
  case HighlightingKind::Namespace:
    return "entity.name.namespace.cpp";
  case HighlightingKind::TemplateParameter:
    return "entity.name.type.template.cpp";
  case HighlightingKind::Primitive:
    return "storage.type.primitive.cpp";
  case HighlightingKind::Macro:
    return "entity.name.function.preprocessor.cpp";
  }
  llvm_unreachable("unhandled HighlightingKind");
}

} // namespace clangd
} // namespace clang
