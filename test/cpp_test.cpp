

#include "collections/meta.h"


#include <iostream>

struct Node {
    GC::Ptr<Node> next;
    ptr::VSharedPtr<Node> prev;

   
    ~Node() {
        std::cout << "Node destroyed\n";
    }
};
//void thread_safe() {
//
//
//    class BankAccount {
//        double balance_;
//        std::string owner_;
//
//    public:
//        BankAccount(std::string owner, double balance)
//            : owner_(std::move(owner)), balance_(balance) {
//            std::cout << std::format("💰 Account for {} created: ${:.2f}\n", owner_, balance_);
//        }
//
//        ~BankAccount() {
//            std::cout << std::format("🏦 Account closed: ${:.2f}\n", balance_);
//        }
//
//        void deposit(double amount) {
//            balance_ += amount;
//            std::cout << std::format("  ➕ Deposited ${:.2f} → Balance: ${:.2f}\n",
//                amount, balance_);
//        }
//
//        bool withdraw(double amount) {
//            if (balance_ >= amount) {
//                balance_ -= amount;
//                std::cout << std::format("  ➖ Withdrew ${:.2f} → Balance: ${:.2f}\n",
//                    amount, balance_);
//                return true;
//            }
//            std::cout << std::format("  ❌ Insufficient funds for ${:.2f}\n", amount);
//            return false;
//        }
//
//        double balance() const { return balance_; }
//    };
//
//    auto account = ptr::VMakeShared<BankAccount, ptr::meta::ThreadMode::True>("Alice", 1000.0);
//
//    std::vector<std::thread> threads;
//
//    // 5 threads depositing
//    for (int i = 0; i < 5; ++i) {
//        threads.emplace_back([account]() {
//            account->deposit(100.0);  // Auto-locked!
//            });
//    }
//
//    // 3 threads withdrawing
//    for (int i = 0; i < 3; ++i) {
//        threads.emplace_back([account]() {
//            account->withdraw(50.0);  // Auto-locked!
//            });
//    }
//
//    for (auto& t : threads) t.join();
//
//    std::cout << std::format("\n💵 Final balance: ${:.2f}\n", account->balance());
//    std::cout << "✅ All transactions thread-safe!\n";
//}
int main() {
    /*{
        thread_safe();
    }*/

    {
        // Cyclic dependency safe pure Garbage collector
        auto a = GC::New<Node>();
        auto b = GC::New<Node>();
        a->next = b;
        b->next = a;


        
       /* {
            auto x = ptr::VMakeShared<Node>();
            auto y = ptr::VMakeShared<Node>();
            x->prev.weak(y);
            y->prev.weak(x);

       }*/
        // for Array types
        /*
        auto arr = ptr::VMakeShared<int[]>(5);
        for (int i = 0; i < 5; ++i)
            arr[i] = i * 10;
        */
    }
    std::cout << "Exiting\n";
}
