/*
 * Clang plugin for PostgreSQL to warn about suspicious misuse of types defined
 * via typedef, e.g. BlockNumber and Buffer. Such validation could not be done
 * without false positives, thus the plugin has to be tuned via suppression
 * rules for types, pair of types and functions.
 */

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace {

class TypedefCheckConsumer : public ASTConsumer {
  CompilerInstance &Instance;

public:
  TypedefCheckConsumer(CompilerInstance &Instance) : Instance(Instance) {}

  void HandleTranslationUnit(ASTContext& context) override {
    struct Visitor : public RecursiveASTVisitor<Visitor> {
      std::set<StringRef> IgnoredTypes = {"size_t", "uintptr_t",
                                          "float8", "uint8", "int8",
                                          "uint16", "int16",
                                          "uint32", "int32",
                                          "uint64", "int64",
                                          "__m128i", "Vector8",
                                          "__off_t", "__off64_t",
                                          "__uid_t", "__mode_t",
                                          "__gid_t", "key_t", "__pid_t",
                                          "__time_t", "Datum",
                                          "yy_size_t", "pg_wchar",
                                          "wchar_t", "fmStringInfo"};

      std::set<std::string> IgnoredFunctions = {"FunctionalCall1Coll", "FunctionalCall2Coll",
                                                "fmgr_info_cxt", "fmgr_info",
                                                "check_amproc_signature", "format_procedure",
                                                "check_amoptsproc_signature",
                                                "check_hash_func_signature"};

      std::set<std::pair<StringRef, StringRef>> IgnoredPairs = {
        std::pair<StringRef, StringRef>("RegProcedure", "Oid"),
        std::pair<StringRef, StringRef>("Oid", "RegProcedure"),
        std::pair<StringRef, StringRef>("Timestamp", "TimestampTz"),
        std::pair<StringRef, StringRef>("TimestampTz", "Timestamp"),
      };

      Visitor(DiagnosticsEngine &Diags) : Diags(Diags) {}

      bool VisitCallExpr(CallExpr *expr) {
        const auto funcDecl = expr->getDirectCallee();
        unsigned DiagID = Diags.getCustomDiagID(DiagnosticsEngine::Warning,
                                            "Typedef check: Expected %0, got %1 in %2");

        if (funcDecl == nullptr)
            return true;

        for (unsigned int i = 0; i != expr->getNumArgs(); ++i) {
            /* Declared argument */
            ParmVarDecl const* argDecl;
            QualType argDeclT;
            QualType argDeclTypedefT;
            TypedefNameDecl const* argDeclTypedef;

            /* Actually provided value */
            Expr *arg;
            QualType argT;
            QualType argTypedefT;
            TypedefNameDecl const* argTypedef;

            arg = expr->getArg(i)->IgnoreParenImpCasts();
            argT = arg->getType();

            if (i >= funcDecl->getNumParams())
                continue;

            argDecl = funcDecl->getParamDecl(i);
            argDeclT = argDecl->getOriginalType();

            if (argT.isNull())
                continue;

            if (argDeclT.isNull())
                continue;

            /* Verify if we deal with a typedef */
            if (auto const t = argT.getTypePtr()->getAs<TypedefType>()) {
                argTypedef = t->getDecl();
                argTypedefT = t->desugar();
            }
            else
                continue;

            if (auto const t = argDeclT.getTypePtr()->getAs<TypedefType>()) {
                argDeclTypedef = t->getDecl();
                argDeclTypedefT = t->desugar();
            }
            else
                continue;

            /*
             * If both types (actual argument type and declared one) are not
             * builtin and could be desugarized into the same type, ignore it.
             */
            if (!argDeclTypedefT->isBuiltinType() &&
                !argTypedefT->isBuiltinType() &&
                (argDeclTypedefT.getAsString() == argTypedefT.getAsString()))
                continue;

            /*
             * If one desugarized type (either the declared or actual) is the
             * same as not desugarized another one, ignore it.
             */
            if (argDeclTypedefT.getAsString() == argTypedef->getName())
                continue;

            if (argTypedefT.getAsString() == argDeclTypedef->getName())
                continue;

            /* If it is one of the suppressed functions, ignore it. */
            if (IgnoredFunctions.find(funcDecl->getNameInfo().getAsString()) != IgnoredFunctions.end())
                continue;

            /* If it is one of the suppressed types, ignore it. */
            if (IgnoredTypes.find(argTypedef->getName()) != IgnoredTypes.end())
                continue;

            if (IgnoredTypes.find(argDeclTypedef->getName()) != IgnoredTypes.end())
                continue;

            /* If it is one of the suppressed pair of types, ignore it. */
            if (IgnoredPairs.find(std::pair<StringRef, StringRef>(argTypedef->getName(), argDeclTypedef->getName())) != IgnoredPairs.end())
                continue;

            if (argTypedef->getName() != argDeclTypedef->getName())
            {
                Diags.Report(arg->getExprLoc(), DiagID)
                    << argDecl->getOriginalType()
                    << arg->getType()
                    << funcDecl->getNameInfo().getAsString();

            }
        }

        return true;
      }

      std::set<FunctionDecl*> LateParsedDecls;
      DiagnosticsEngine &Diags;

    } v(Instance.getDiagnostics());
    v.TraverseDecl(context.getTranslationUnitDecl());
  }
};

class TypedefCheckAction : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    return std::make_unique<TypedefCheckConsumer>(CI);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
      return true;
  }

  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "TypedefCheck plugin for PostgreSQL warns about suspicious "
           "misuse of types defined via typedef.\n";
  }

};

}

static FrontendPluginRegistry::Add<TypedefCheckAction>
X("typedef-check", "check typedefs");
