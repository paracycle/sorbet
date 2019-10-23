#include "dsl/flatten.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "core/core.h"

#include <utility>

using namespace std;

namespace sorbet::dsl {

class FlattenWalk {
private:
public:
    FlattenWalk() {
        newMethodSet();
    }
    ~FlattenWalk() {
        ENFORCE(methodScopes.empty());
    }

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> classDef) {
        newMethodSet();
        return classDef;
    }

    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> methodDef) {
        auto &methods = curMethodSet();
        if (skipMethods.contains(methodDef.get())) {
            ENFORCE(!methods.stack.empty());
            methods.stack.back().isStatic = methodDef->isSelf();
            return methodDef;
        }
        bool isStatic = methodDef->isSelf() || (methods.stack.size() > 0 && methods.stack.back().isStatic);
        methods.stack.emplace_back(methods.methods.size(), isStatic);
        methods.methods.emplace_back();
        return methodDef;
    }

    /// Returns `true` if the method is one of the modifier names in Ruby (e.g. 'private' or 'protected' or
    /// similar). This does not need to know about `module_function` because we have already re-written it in a previous
    /// DSL pass.
    bool isMethodModifier(ast::Send &send) {
        auto fun = send.fun;
        return (fun == core::Names::private_() || fun == core::Names::protected_() || fun == core::Names::public_() ||
                fun == core::Names::privateClassMethod()) &&
               send.args.size() == 1 && ast::isa_tree<ast::MethodDef>(send.args[0].get());
    }

    unique_ptr<ast::Send> preTransformSend(core::Context ctx, unique_ptr<ast::Send> send) {
        if (send->fun == core::Names::sig() || isMethodModifier(*send)) {
            auto &methods = curMethodSet();
            bool isStatic = methods.stack.size() > 0 && methods.stack.back().isStatic;
            methods.stack.emplace_back(methods.methods.size(), isStatic);
            methods.methods.emplace_back();

            if (isMethodModifier(*send) && send->args.size() >= 1) {
                skipMethods.insert(send->args[0].get());
            }
        }
        return send;
    }

    unique_ptr<ast::Expression> postTransformSend(core::Context ctx, unique_ptr<ast::Send> send) {
        if (send->fun == core::Names::sig() || isMethodModifier(*send)) {
            auto &methods = curMethodSet();
            ENFORCE(!methods.stack.empty());
            ENFORCE(methods.methods.size() > methods.stack.back().idx);
            ENFORCE(methods.methods[methods.stack.back().idx] == nullptr);

            methods.methods[methods.stack.back().idx] = std::move(send);
            methods.stack.pop_back();

            return make_unique<ast::EmptyTree>();
        } else {
            return send;
        }
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> classDef) {
        classDef->rhs = addMethods(ctx, std::move(classDef->rhs));
        return classDef;
    };

    unique_ptr<ast::Expression> postTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> methodDef) {
        // if this method is contained in a send like `private` or `protected`, then we should not move it, because
        // moving the send will do that for us
        if (skipMethods.contains(methodDef.get())) {
            return methodDef;
        }
        auto &methods = curMethodSet();
        ENFORCE(!methods.stack.empty());
        ENFORCE(methods.methods.size() > methods.stack.back().idx);
        ENFORCE(methods.methods[methods.stack.back().idx] == nullptr);

        if (methods.stack.back().isStatic) {
            methodDef->flags |= ast::MethodDef::SelfMethod;
        }

        methods.methods[methods.stack.back().idx] = std::move(methodDef);
        methods.stack.pop_back();
        return make_unique<ast::EmptyTree>();
    };

    unique_ptr<ast::Expression> addMethods(core::Context ctx, unique_ptr<ast::Expression> tree) {
        auto &methods = curMethodSet().methods;
        if (methods.empty()) {
            ENFORCE(popCurMethodDefs().empty());
            return tree;
        }
        if (methods.size() == 1 && (ast::cast_tree<ast::EmptyTree>(tree.get()) != nullptr)) {
            // It was only 1 method to begin with, put it back
            unique_ptr<ast::Expression> methodDef = std::move(popCurMethodDefs()[0]);
            return methodDef;
        }

        auto insSeq = ast::cast_tree<ast::InsSeq>(tree.get());
        if (insSeq == nullptr) {
            ast::InsSeq::STATS_store stats;
            tree = make_unique<ast::InsSeq>(tree->loc, std::move(stats), std::move(tree));
            return addMethods(ctx, std::move(tree));
        }

        for (auto &method : popCurMethodDefs()) {
            ENFORCE(!!method);
            insSeq->stats.emplace_back(std::move(method));
        }
        return tree;
    }

private:
    ast::ClassDef::RHS_store addMethods(core::Context ctx, ast::ClassDef::RHS_store rhs) {
        if (curMethodSet().methods.size() == 1 && rhs.size() == 1 &&
            (ast::cast_tree<ast::EmptyTree>(rhs[0].get()) != nullptr)) {
            // It was only 1 method to begin with, put it back
            rhs.pop_back();
            rhs.emplace_back(std::move(popCurMethodDefs()[0]));
            return rhs;
        }
        for (auto &method : popCurMethodDefs()) {
            ENFORCE(method.get() != nullptr);
            rhs.emplace_back(std::move(method));
        }
        return rhs;
    }

    vector<unique_ptr<ast::Expression>> popCurMethodDefs() {
        auto ret = std::move(curMethodSet().methods);
        ENFORCE(curMethodSet().stack.empty());
        popCurMethodSet();
        return ret;
    };

    struct MethodData {
        int idx;
        bool isStatic;
        MethodData(int idx, bool isStatic) : idx(idx), isStatic(isStatic){};
    };
    struct Methods {
        vector<unique_ptr<ast::Expression>> methods;
        vector<MethodData> stack;
        Methods() = default;
    };
    void newMethodSet() {
        methodScopes.emplace_back();
    }
    Methods &curMethodSet() {
        ENFORCE(!methodScopes.empty());
        return methodScopes.back();
    }
    void popCurMethodSet() {
        ENFORCE(!methodScopes.empty());
        methodScopes.pop_back();
    }

    struct ClassScope {
        int index;
        ClassScope(int index) : index(index) {};
    };

    // We flatten methods so that we have an arbitrary hierarchy of classes each of which has a flat list of
    // methods. This prevents methods from existing deeper inside the hierarchy, enabling later traversals to stop
    // recursing over the AST once they've reached a method def.
    vector<Methods> methodScopes;
    // this allows us to skip adding methods to the method stack if we are going to add them as part of a larger
    // expression: for example, if we have already seen the send `private(def foo...)` then we'll add the entire send,
    // and not just the method.
    UnorderedSet<ast::Expression *> skipMethods;
};

unique_ptr<ast::Expression> Flatten::patchFile(core::Context ctx, unique_ptr<ast::Expression> tree) {
    FlattenWalk flatten;
    tree = ast::TreeMap::apply(ctx, flatten, std::move(tree));
    tree = flatten.addMethods(ctx, std::move(tree));

    return tree;
}

} // namespace sorbet::dsl