import { BrowserRouter, Routes, Route } from "react-router-dom";
import { AuthProvider } from "./contexts/AuthContext";
import { MachinaPage } from "./pages/MachinaPage";
import "./styles.css";

export default function App() {
  return (
    <AuthProvider>
      <BrowserRouter>
        <Routes>
          <Route path="/" element={<MachinaPage />} />
        </Routes>
      </BrowserRouter>
    </AuthProvider>
  );
}
